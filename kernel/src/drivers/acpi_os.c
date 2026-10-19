#include <uacpi/kernel_api.h>
#include <uacpi/status.h>

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <boot/bootinfo.h>
#include <drivers/pci.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <mm/vmm.h>

#define MMIO_CACHE   64
#define WORK_QUEUE   32
#define FOREVER      0xFFFF

struct mmio_map {
    uint64_t phys;
    uint64_t len;
    uint8_t *virt;
};

struct pci_handle {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
};

#define IO_HANDLE_TAG (1ull << 62)

struct acpi_event {
    spinlock_t lock;
    struct waitqueue wq;
    uint64_t count;
};

struct work_item {
    uacpi_work_handler fn;
    uacpi_handle ctx;
};

struct irq_slot {
    uacpi_interrupt_handler fn;
    uacpi_handle ctx;
    uint8_t irq;
    bool used;
};

static struct mmio_map mmio_maps[MMIO_CACHE];
static int mmio_count;
static spinlock_t mmio_lock;

static struct work_item work[WORK_QUEUE];
static uint32_t work_head, work_tail, work_running;
static spinlock_t work_lock;
static struct waitqueue work_wq, work_done_wq;
static struct thread *work_thread;

static struct irq_slot irq_slots[4];

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address)
{
    if (!boot_info.rsdp)
        return UACPI_STATUS_NOT_FOUND;
    *out_rsdp_address = (uint64_t)boot_info.rsdp - hhdm_offset;
    return UACPI_STATUS_OK;
}

static bool hhdm_covers(uint64_t phys, uint64_t len)
{
    for (uint64_t p = phys & ~(uint64_t)(PAGE_SIZE - 1); p < phys + len; p += PAGE_SIZE)
        if (!vmm_virt_to_phys((uint64_t)phys_to_virt(p)))
            return false;
    return true;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    if (!len)
        len = 1;
    if (hhdm_covers(addr, len))
        return phys_to_virt(addr);
    uint64_t fl = spin_lock_irqsave(&mmio_lock);
    for (int i = 0; i < mmio_count; i++) {
        struct mmio_map *m = &mmio_maps[i];
        if (addr >= m->phys && addr + len <= m->phys + m->len) {
            spin_unlock_irqrestore(&mmio_lock, fl);
            return m->virt + (addr - m->phys);
        }
    }
    spin_unlock_irqrestore(&mmio_lock, fl);
    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = PAGE_ALIGN_UP(addr + len);
    uint8_t *v = vmm_map_mmio(start, end - start);
    fl = spin_lock_irqsave(&mmio_lock);
    if (mmio_count < MMIO_CACHE)
        mmio_maps[mmio_count++] = (struct mmio_map){ start, end - start, v };
    spin_unlock_irqrestore(&mmio_lock, fl);
    return v + (addr - start);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len)
{
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg)
{
    char line[192];
    size_t n = strlcpy(line, msg, sizeof(line));
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    switch (level) {
    case UACPI_LOG_ERROR: log_error("uacpi", "%s", line); break;
    case UACPI_LOG_WARN:  log_warn("uacpi", "%s", line); break;
    case UACPI_LOG_INFO:  log_info("uacpi", "%s", line); break;
    default: break;
    }
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle)
{
    if (address.segment != 0)
        return UACPI_STATUS_NOT_FOUND;
    struct pci_handle *h = kmalloc(sizeof(*h));
    if (!h)
        return UACPI_STATUS_OUT_OF_MEMORY;
    h->bus = address.bus;
    h->slot = address.device;
    h->func = address.function;
    *out_handle = h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    kfree(handle);
}

static uint32_t pci_dword(struct pci_handle *h, uacpi_size offset)
{
    return pci_read32(h->bus, h->slot, h->func, (uint8_t)(offset & 0xFC));
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value)
{
    if (offset > 0xFF)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *value = (uint8_t)(pci_dword(device, offset) >> ((offset & 3) * 8));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value)
{
    if (offset > 0xFE || (offset & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *value = (uint16_t)(pci_dword(device, offset) >> ((offset & 2) * 8));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value)
{
    if (offset > 0xFC || (offset & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *value = pci_dword(device, offset);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_merge(uacpi_handle device, uacpi_size offset, uint32_t value, uint32_t mask)
{
    struct pci_handle *h = device;
    uint32_t shift = (uint32_t)(offset & 3) * 8;
    uint32_t old = pci_dword(h, offset);
    uint32_t v = (old & ~(mask << shift)) | ((value & mask) << shift);
    pci_write32(h->bus, h->slot, h->func, (uint8_t)(offset & 0xFC), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value)
{
    if (offset > 0xFF)
        return UACPI_STATUS_INVALID_ARGUMENT;
    return pci_merge(device, offset, value, 0xFF);
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value)
{
    if (offset > 0xFE || (offset & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;
    return pci_merge(device, offset, value, 0xFFFF);
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value)
{
    if (offset > 0xFC || (offset & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct pci_handle *h = device;
    pci_write32(h->bus, h->slot, h->func, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
{
    if (base > 0xFFFF || len > 0x10000 - base)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_handle = (uacpi_handle)(uintptr_t)(IO_HANDLE_TAG | ((uint64_t)len << 16) | base);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    (void)handle;
}

static bool io_ok(uacpi_handle handle, uacpi_size offset, uacpi_size width, uint16_t *port)
{
    uint64_t v = (uint64_t)(uintptr_t)handle;
    if (!(v & IO_HANDLE_TAG))
        return false;
    uint64_t base = v & 0xFFFF;
    uint64_t len = (v >> 16) & 0x1FFFF;
    if (offset + width > len)
        return false;
    *port = (uint16_t)(base + offset);
    return true;
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 1, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inb(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 2, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inw(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 4, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inl(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 1, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outb(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 2, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outw(port, in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value)
{
    uint16_t port;
    if (!io_ok(handle, offset, 4, &port))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outl(port, in_value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return kmalloc(size ? size : 1);
}

void uacpi_kernel_free(void *mem)
{
    if (mem)
        kfree(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return time_now_us() * 1000;
}

void uacpi_kernel_stall(uacpi_u8 usec)
{
    time_udelay(usec);
}

void uacpi_kernel_sleep(uacpi_u64 msec)
{
    if (sched_enabled() && interrupts_enabled())
        thread_sleep_ms(msec);
    else
        time_udelay(msec * 1000);
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    struct mutex *m = kzalloc(sizeof(*m));
    if (m)
        mutex_init_private(m, "uacpi");
    return m;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
    mutex_deinit(handle);
    kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout)
{
    struct mutex *m = handle;
    if (timeout == FOREVER) {
        mutex_lock(m);
        return UACPI_STATUS_OK;
    }
    uint64_t deadline = time_ticks() + timeout;
    for (;;) {
        if (mutex_trylock(m))
            return UACPI_STATUS_OK;
        if (time_ticks() >= deadline)
            return UACPI_STATUS_TIMEOUT;
        thread_sleep_ms(1);
    }
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
    mutex_unlock(handle);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    struct acpi_event *e = kzalloc(sizeof(*e));
    if (e) {
        spinlock_init(&e->lock);
        waitqueue_init(&e->wq);
    }
    return e;
}

void uacpi_kernel_free_event(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout)
{
    struct acpi_event *e = handle;
    uint64_t deadline = time_ticks() + timeout;
    uint64_t fl = spin_lock_irqsave(&e->lock);
    while (!e->count) {
        if (timeout != FOREVER) {
            uint64_t now = time_ticks();
            if (now >= deadline) {
                spin_unlock_irqrestore(&e->lock, fl);
                return UACPI_FALSE;
            }
            wait_sleep_timeout(&e->wq, &e->lock, deadline - now);
        } else {
            wait_sleep(&e->wq, &e->lock);
        }
    }
    e->count--;
    spin_unlock_irqrestore(&e->lock, fl);
    return UACPI_TRUE;
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
    struct acpi_event *e = handle;
    uint64_t fl = spin_lock_irqsave(&e->lock);
    e->count++;
    spin_unlock_irqrestore(&e->lock, fl);
    wake_up(&e->wq);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
    struct acpi_event *e = handle;
    uint64_t fl = spin_lock_irqsave(&e->lock);
    e->count = 0;
    spin_unlock_irqrestore(&e->lock, fl);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return thread_current();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return irq_save();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    irq_restore(state);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req)
{
    if (req->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        log_error("acpi", "firmware pediu parada fatal: tipo %u, codigo %u, arg %lu", req->fatal.type,
                  req->fatal.code, (uint64_t)req->fatal.arg);
    else
        log_warn("acpi", "breakpoint do AML ignorado");
    return UACPI_STATUS_OK;
}

static void irq_trampoline(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    struct irq_slot *s = ctx;
    s->fn(s->ctx);
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn, uacpi_handle ctx,
                                                    uacpi_handle *out_irq_handle)
{
    if (irq >= VECTOR_IRQ_COUNT)
        return UACPI_STATUS_UNIMPLEMENTED;
    for (size_t i = 0; i < ARRAY_SIZE(irq_slots); i++) {
        struct irq_slot *s = &irq_slots[i];
        if (s->used)
            continue;
        s->fn = fn;
        s->ctx = ctx;
        s->irq = (uint8_t)irq;
        s->used = true;
        interrupts_register((uint8_t)(VECTOR_IRQ_BASE + irq), irq_trampoline, s);
        irq_enable((uint8_t)irq);
        *out_irq_handle = s;
        return UACPI_STATUS_OK;
    }
    return UACPI_STATUS_OUT_OF_MEMORY;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle irq_handle)
{
    (void)fn;
    struct irq_slot *s = irq_handle;
    irq_disable(s->irq);
    interrupts_unregister((uint8_t)(VECTOR_IRQ_BASE + s->irq));
    s->used = false;
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    spinlock_t *l = kzalloc(sizeof(*l));
    if (l)
        spinlock_init(l);
    return l;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
    return spin_lock_irqsave(handle);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags)
{
    spin_unlock_irqrestore(handle, flags);
}

static void work_loop(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&work_lock);
        while (work_head == work_tail)
            wait_sleep(&work_wq, &work_lock);
        struct work_item it = work[work_head % WORK_QUEUE];
        work_head++;
        work_running++;
        spin_unlock_irqrestore(&work_lock, fl);
        it.fn(it.ctx);
        fl = spin_lock_irqsave(&work_lock);
        work_running--;
        spin_unlock_irqrestore(&work_lock, fl);
        wake_up_all(&work_done_wq);
    }
}

void acpi_os_init(void)
{
    spinlock_init(&mmio_lock);
    spinlock_init(&work_lock);
    waitqueue_init(&work_wq);
    waitqueue_init(&work_done_wq);
    work_thread = thread_create("acpi-work", work_loop, NULL);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler fn, uacpi_handle ctx)
{
    (void)type;
    if (!work_thread)
        return UACPI_STATUS_INTERNAL_ERROR;
    uint64_t fl = spin_lock_irqsave(&work_lock);
    if (work_tail - work_head >= WORK_QUEUE) {
        spin_unlock_irqrestore(&work_lock, fl);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    work[work_tail % WORK_QUEUE] = (struct work_item){ fn, ctx };
    work_tail++;
    spin_unlock_irqrestore(&work_lock, fl);
    wake_up(&work_wq);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    uint64_t fl = spin_lock_irqsave(&work_lock);
    while (work_head != work_tail || work_running)
        wait_sleep_timeout(&work_done_wq, &work_lock, 10);
    spin_unlock_irqrestore(&work_lock, fl);
    return UACPI_STATUS_OK;
}
