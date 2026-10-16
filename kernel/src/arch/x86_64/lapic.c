#include <arch/x86_64/lapic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <mm/vmm.h>
#include <drivers/pit.h>
#include <kernel/log.h>

#define APIC_BASE_ENABLE   (1ull << 11)
#define APIC_BASE_EXTD     (1ull << 10)
#define APIC_BASE_ADDR_MASK 0x000FFFFFFFFFF000ull

static bool enabled = false;
static bool x2apic = false;
static uint64_t timer_hz = 0;
static volatile uint32_t *mmio = NULL;

static inline uint64_t lapic_read(uint32_t reg)
{
    if (x2apic)
        return rdmsr(X2APIC_MSR_BASE + reg);
    return mmio[reg * 4];
}

static inline void lapic_write(uint32_t reg, uint64_t value)
{
    if (x2apic)
        wrmsr(X2APIC_MSR_BASE + reg, value);
    else
        mmio[reg * 4] = (uint32_t)value;
}

bool lapic_init(void)
{
    struct cpuid_result r = cpuid(1, 0);
    if (!(r.edx & CPUID_1_EDX_APIC)) {
        log_warn("lapic", "CPU sem APIC local");
        return false;
    }

    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);

    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }

    if (r.ecx & CPUID_1_ECX_X2APIC) {
        if (!(base & APIC_BASE_EXTD)) {
            base |= APIC_BASE_EXTD;
            wrmsr(MSR_IA32_APIC_BASE, base);
        }
        x2apic = true;
    } else {
        uint64_t phys = base & APIC_BASE_ADDR_MASK;
        mmio = vmm_map_mmio(phys, 4096);
        log_info("lapic", "xAPIC MMIO %016lx mapeado em %p", phys, (void *)mmio);
    }
    enabled = true;

    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | VECTOR_SPURIOUS);
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_DELIVERY_NMI | LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);

    uint32_t version = (uint32_t)lapic_read(LAPIC_REG_VERSION);
    log_info("lapic", "%s ligado: id=%u versao=0x%02x lvt_entries=%u, LINT0=ExtINT",
             x2apic ? "x2APIC" : "xAPIC", lapic_id(), version & 0xFF,
             ((version >> 16) & 0xFF) + 1);
    return true;
}

bool lapic_is_enabled(void)
{
    return enabled;
}

bool lapic_is_x2apic(void)
{
    return x2apic;
}

uint32_t lapic_id(void)
{
    if (!enabled)
        return 0;
    uint32_t id = (uint32_t)lapic_read(LAPIC_REG_ID);
    return x2apic ? id : id >> 24;
}

void lapic_eoi(void)
{
    if (enabled)
        lapic_write(LAPIC_REG_EOI, 0);
}

void lapic_set_lint0_extint(bool enable)
{
    if (enabled)
        lapic_write(LAPIC_REG_LVT_LINT0, enable ? LAPIC_LVT_DELIVERY_EXTINT : LAPIC_LVT_MASKED);
}

#define TIMER_DIV_16 0x3

bool lapic_timer_calibrate(void)
{
    if (!enabled)
        return false;

    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    pit_channel2_wait_ms(10);

    uint32_t remaining = (uint32_t)lapic_read(LAPIC_REG_TIMER_CUR);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    uint64_t elapsed = 0xFFFFFFFFu - remaining;
    if (elapsed < 1000) {
        log_warn("lapic", "timer nao avancou durante a calibracao");
        return false;
    }
    timer_hz = elapsed * 100;
    log_info("lapic", "timer calibrado: %lu ticks/s (div 16)", timer_hz);
    return true;
}

uint64_t lapic_timer_frequency(void)
{
    return timer_hz;
}

void lapic_timer_start_periodic(uint8_t vector, uint32_t hz)
{
    if (!enabled || !timer_hz)
        return;
    uint32_t initial = (uint32_t)(timer_hz / hz);
    if (initial == 0)
        initial = 1;
    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_REG_LVT_TIMER, vector | LAPIC_LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INIT, initial);
}

void lapic_timer_stop(void)
{
    if (!enabled)
        return;
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
}

void lapic_enable_this_cpu(void)
{
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        base |= APIC_BASE_ENABLE;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    if (x2apic && !(base & APIC_BASE_EXTD)) {
        base |= APIC_BASE_EXTD;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | VECTOR_SPURIOUS);
    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED);
}

#define ICR_DEST_ALL_BUT_SELF (3u << 18)
#define ICR_DELIVERY_NMI      (4u << 8)
#define ICR_PENDING           (1u << 12)

static void icr_write(uint32_t dest, uint32_t low)
{
    uint64_t flags = irq_save();
    if (x2apic) {
        wrmsr(X2APIC_MSR_BASE + LAPIC_REG_ICR, ((uint64_t)dest << 32) | low);
    } else {
        mmio[(LAPIC_REG_ICR + 1) * 4] = dest << 24;
        mmio[LAPIC_REG_ICR * 4] = low;
        while (mmio[LAPIC_REG_ICR * 4] & ICR_PENDING)
            pause();
    }
    irq_restore(flags);
}

void lapic_send_ipi(uint32_t dest, uint8_t vector)
{
    if (enabled)
        icr_write(dest, vector);
}

void lapic_send_ipi_others(uint8_t vector)
{
    if (enabled)
        icr_write(0, ICR_DEST_ALL_BUT_SELF | vector);
}

void lapic_send_nmi(uint32_t dest)
{
    if (enabled)
        icr_write(dest, ICR_DELIVERY_NMI);
}
