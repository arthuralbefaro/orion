#include <drivers/usb.h>
#include <drivers/pci.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <lib/string.h>

int32_t xhci_start(uint8_t *mmio);
size_t xhci_report(uint8_t *buf, size_t cap);

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

uint64_t usb_now_us(void)
{
    return time_now_us();
}

void usb_sleep_ms(uint64_t ms)
{
    if (interrupts_enabled() && sched_enabled()) {
        thread_sleep_ms(ms);
        return;
    }
    uint64_t end = time_now_us() + ms * 1000;
    while (time_now_us() < end)
        pause();
}

void usb_log(const uint8_t *msg, size_t len)
{
    log_info("usb", "%.*s", (int)len, (const char *)msg);
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
    int32_t n = xhci_start(mmio);
    present = n >= 0;
    if (present)
        log_info("usb", "%d dispositivo(s) USB", n);
    return present;
}

size_t usb_report(char *buf, size_t cap)
{
    return xhci_report((uint8_t *)buf, cap);
}
