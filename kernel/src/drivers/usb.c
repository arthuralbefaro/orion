#include <drivers/usb.h>
#include <drivers/pci.h>
#include <drivers/keyboard.h>
#include <drivers/mouse.h>
#include <fs/block.h>
#include <kernel/mutex.h>
#include <lib/printf.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <lib/string.h>

int32_t xhci_start(uint8_t *mmio, bool irq);
void xhci_irq(void);
int32_t xhci_msc_test_stall(uint32_t disk);
int32_t xhci_msc_test_timeout(uint32_t disk);
void xhci_msc_release(uint32_t disk);
size_t xhci_report(uint8_t *buf, size_t cap);
int32_t xhci_hid_count(void);
void xhci_poll(void);
bool xhci_msc_read(uint32_t disk, uint64_t lba, uint32_t count, uint8_t *buf);
bool xhci_msc_write(uint32_t disk, uint64_t lba, uint32_t count, const uint8_t *buf);

#define USB_DISKS 16

struct usb_disk {
    uint32_t index;
    bool used;
    struct blockdev *dev;
    char name[BLOCK_NAME_MAX];
    struct mutex lock;
    char lock_name[12];
};

static struct usb_disk disks[USB_DISKS];
static spinlock_t wq_lock = SPINLOCK_INIT;
static struct waitqueue wq = WAITQUEUE_INIT;
static bool irq_on;

static bool can_sleep(void)
{
    return interrupts_enabled() && sched_enabled();
}

void *usb_dma_alloc(size_t pages, uint64_t *phys)
{
    uint64_t p = pmm_alloc_pages(pages);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    *phys = p;
    return v;
}

void usb_dma_free(uint64_t phys, size_t pages)
{
    pmm_free_pages(phys, pages);
}

uint64_t usb_now_us(void)
{
    return time_now_us();
}

void usb_sleep_ms(uint64_t ms)
{
    if (can_sleep()) {
        thread_sleep_ms(ms);
        return;
    }
    uint64_t end = time_now_us() + ms * 1000;
    while (time_now_us() < end)
        pause();
}

uint64_t usb_irq_save(void)
{
    return irq_save();
}

void usb_irq_restore(uint64_t flags)
{
    irq_restore(flags);
}

void usb_wait_ms(uint64_t ms)
{
    if (!can_sleep()) {
        pause();
        return;
    }
    uint64_t flags = spin_lock_irqsave(&wq_lock);
    wait_sleep_timeout(&wq, &wq_lock, ms);
    spin_unlock_irqrestore(&wq_lock, flags);
}

void usb_wake(void)
{
    uint64_t flags = spin_lock_irqsave(&wq_lock);
    wake_up_all(&wq);
    spin_unlock_irqrestore(&wq_lock, flags);
}

void usb_yield(void)
{
    if (can_sleep())
        thread_yield();
    else
        pause();
}

void usb_log(const uint8_t *msg, size_t len)
{
    log_info("usb", "%.*s", (int)len, (const char *)msg);
}

void usb_key(uint8_t scancode, bool extended, bool pressed)
{
    if (extended)
        keyboard_feed(0xE0);
    keyboard_feed((uint8_t)(scancode | (pressed ? 0 : 0x80)));
}

void usb_mouse(int32_t dx, int32_t dy, int32_t dz, uint8_t buttons)
{
    mouse_inject(dx, dy, dz, buttons);
}

static bool msc_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct usb_disk *d = dev->priv;
    mutex_lock(&d->lock);
    bool ok = xhci_msc_read(d->index, lba, count, buf);
    mutex_unlock(&d->lock);
    if (!ok)
        log_error("usb", "%s: erro lendo LBA %lu", dev->name, lba);
    return ok;
}

static bool msc_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct usb_disk *d = dev->priv;
    mutex_lock(&d->lock);
    bool ok = xhci_msc_write(d->index, lba, count, buf);
    mutex_unlock(&d->lock);
    if (!ok)
        log_error("usb", "%s: erro escrevendo LBA %lu", dev->name, lba);
    return ok;
}

static void msc_release(struct blockdev *dev)
{
    struct usb_disk *d = dev->priv;
    xhci_msc_release(d->index);
    d->dev = NULL;
}

void usb_disk_gone(uint32_t index)
{
    if (index >= USB_DISKS || !disks[index].dev)
        return;
    struct usb_disk *d = &disks[index];
    d->used = false;
    log_warn("usb", "%s: pendrive desconectado; leitura e escrita passam a dar EIO", d->name);
    blockdev_remove(d->dev);
}

void usb_disk_add(uint32_t index, uint64_t blocks, const uint8_t *name, size_t len)
{
    if (index >= USB_DISKS) {
        log_warn("usb", "pendrive %u ignorado: so cabem %u registros de pendrive por boot", index, USB_DISKS);
        return;
    }
    char dev_name[BLOCK_NAME_MAX];
    for (char c = 'a'; c <= 'z'; c++) {
        ksnprintf(dev_name, sizeof(dev_name), "sd%c", c);
        if (!blockdev_find(dev_name))
            break;
    }
    struct usb_disk *d = &disks[index];
    d->index = index;
    ksnprintf(d->lock_name, sizeof(d->lock_name), "usbdisk%u", index);
    mutex_init(&d->lock, d->lock_name);
    struct blockdev *dev = blockdev_register(dev_name, 512, blocks, msc_read, msc_write, d);
    if (!dev)
        return;
    dev->release = msc_release;
    d->dev = dev;
    d->used = true;
    ksnprintf(d->name, sizeof(d->name), "%s", dev_name);
    log_info("usb", "%s: pendrive '%.*s', %lu setores (%lu MiB)", dev_name, (int)len, (const char *)name, blocks,
             blocks / 2048);
    blockdev_scan_partitions(dev);
}

static int run_test(const char *name, int32_t (*test)(uint32_t))
{
    for (uint32_t i = 0; i < USB_DISKS; i++) {
        struct usb_disk *d = &disks[i];
        if (!d->used || strcmp(d->name, name) != 0)
            continue;
        mutex_lock(&d->lock);
        int r = test(d->index);
        mutex_unlock(&d->lock);
        return r;
    }
    return -1;
}

int usb_test_stall(const char *name)
{
    return run_test(name, xhci_msc_test_stall);
}

int usb_test_timeout(const char *name)
{
    return run_test(name, xhci_msc_test_timeout);
}

static void xhci_irq_handler(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    xhci_irq();
}

static void poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        xhci_poll();
        thread_sleep_ms(irq_on ? 100 : 4);
    }
}

static bool present;

bool usb_init(void)
{
    const struct pci_device *dev = NULL;
    for (uint32_t i = 0; (dev = pci_find_class(0x0C, 0x03, i)) && dev->prog_if != 0x30; i++)
        ;
    if (!dev)
        return false;
    uint64_t base = pci_bar_address(dev, 0);
    if (!base) {
        log_warn("usb", "controlador xHCI sem BAR de memoria");
        return false;
    }
    pci_enable_bus_master(dev);
    uint8_t *mmio = vmm_map_mmio(base, 0x10000);
    log_info("usb", "controlador xHCI %04x:%04x em %02x:%02x.%u", dev->vendor, dev->device, dev->bus, dev->slot,
             dev->func);
    int vector = interrupts_alloc_vector(xhci_irq_handler, NULL, "xhci");
    uint32_t cpu = irq_next_cpu();
    const char *how = "MSI-X";
    irq_on = vector >= 0 && pci_enable_msix(dev, 0, (uint8_t)vector, cpu);
    if (!irq_on && vector >= 0) {
        how = "MSI";
        irq_on = pci_enable_msi(dev, (uint8_t)vector, cpu);
    }
    if (!irq_on && vector >= 0)
        interrupts_unregister((uint8_t)vector);
    if (irq_on)
        log_info("usb", "xHCI por %s no vetor %d, CPU %u", how, vector, cpu);
    else
        log_warn("usb", "xHCI sem MSI nem MSI-X; os eventos vem por consulta");
    int32_t n = xhci_start(mmio, irq_on);
    present = n >= 0;
    if (present)
        log_info("usb", "%d dispositivo(s) USB", n);
    if (present)
        thread_create("usbd", poll_thread, NULL);
    return present;
}

size_t usb_report(char *buf, size_t cap)
{
    return xhci_report((uint8_t *)buf, cap);
}
