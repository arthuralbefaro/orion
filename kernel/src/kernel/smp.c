#include <kernel/smp.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/syscall.h>
#include <kernel/spinlock.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/percpu.h>
#include <mm/vmm.h>
#include <limine.h>

#define WATCHDOG_SECONDS 5

static volatile uint32_t aps_online;
static bool active;
static spinlock_t tlb_lock = SPINLOCK_INIT;
static volatile uint32_t tlb_pending;
static uint64_t tlb_sync_count;
static volatile int32_t report_cpu = -1;
static volatile bool report_done;
static volatile uint32_t report_taken;
static const char *volatile report_why;

static void ipi_halt(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    halt_forever();
}

static void ipi_tlb(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    write_cr3(read_cr3());
}

static void ipi_tlb_sync(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    write_cr3(read_cr3());
    __atomic_fetch_sub(&tlb_pending, 1, __ATOMIC_SEQ_CST);
}

static void nmi_handler(struct interrupt_frame *f, void *ctx)
{
    (void)ctx;
    if ((int32_t)percpu_cpu_id() == report_cpu) {
        kprintf("\n--- CPU %u %s ---\n", percpu_cpu_id(), report_why ? report_why : "");
        interrupts_dump(f);
        report_done = true;
        halt_forever();
    }
    log_warn("nmi", "NMI inesperada na CPU %u em RIP=%016lx", percpu_cpu_id(), f->rip);
}

static void ap_entry(struct limine_mp_info *info)
{
    uint32_t idx = (uint32_t)info->extra_argument;

    gdt_init_cpu(idx);
    idt_load();
    percpu_init_ap(idx, info->lapic_id);
    write_cr3(vmm_kernel_pml4());
    lapic_enable_this_cpu();
    syscall_init_cpu();
    sched_init_ap();
    time_init_ap();

    __atomic_fetch_add(&aps_online, 1, __ATOMIC_SEQ_CST);

    for (;;) {
        sti();
        hlt();
    }
}

void smp_init(void)
{
    interrupts_register(VECTOR_IPI_HALT, ipi_halt, NULL);
    interrupts_register(VECTOR_IPI_TLB, ipi_tlb, NULL);
    interrupts_register(VECTOR_IPI_TLB_SYNC, ipi_tlb_sync, NULL);
    interrupts_register(EXC_NMI, nmi_handler, NULL);

    struct limine_mp_response *mp = boot_info.mp;
    if (!mp || mp->cpu_count <= 1) {
        log_info("smp", "1 CPU");
        return;
    }

    uint32_t idx = 1;
    for (uint64_t i = 0; i < mp->cpu_count && idx < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id)
            continue;
        if (!gdt_alloc_ist(idx)) {
            log_warn("smp", "sem memoria para as pilhas de emergencia da CPU %u", idx);
            break;
        }
        info->extra_argument = idx++;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    uint32_t expected = idx - 1;
    uint64_t deadline = time_now_us() + 2000000;
    while (aps_online < expected && time_now_us() < deadline)
        pause();

    active = aps_online > 0;
    log_info("smp", "%u CPU(s) online de %lu no MADT (x2APIC nos APs: %s)",
             aps_online + 1, mp->cpu_count,
             (mp->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC) ? "sim" : "nao");
    if (aps_online < expected)
        log_warn("smp", "%u AP(s) nao responderam", expected - aps_online);
}

uint32_t smp_cpu_count(void)
{
    return percpu_count();
}

bool smp_active(void)
{
    return active;
}

void smp_flush_tlb_others(void)
{
    if (active)
        lapic_send_ipi_others(VECTOR_IPI_TLB);
}

void smp_tlb_shootdown_sync(void)
{
    if (!active)
        return;
    ASSERT(interrupts_enabled());
    spin_lock(&tlb_lock);
    __atomic_store_n(&tlb_pending, percpu_count() - 1, __ATOMIC_SEQ_CST);
    lapic_send_ipi_others(VECTOR_IPI_TLB_SYNC);
    while (__atomic_load_n(&tlb_pending, __ATOMIC_SEQ_CST) != 0)
        pause();
    tlb_sync_count++;
    spin_unlock(&tlb_lock);
}

void smp_halt_others(void)
{
    if (active)
        lapic_send_ipi_others(VECTOR_IPI_HALT);
}

void smp_report_cpu(uint32_t cpu, const char *why)
{
    struct percpu *c = percpu_by_index(cpu);
    if (!active || !c || !c->online || cpu == percpu_cpu_id())
        return;
    if (__atomic_exchange_n(&report_taken, 1, __ATOMIC_SEQ_CST))
        return;
    report_why = why;
    report_done = false;
    report_cpu = (int32_t)cpu;
    lapic_send_nmi(c->lapic_id);
    uint64_t end = time_now_us() + 2000000;
    while (!report_done && time_now_us() < end)
        pause();
}

void smp_watchdog_tick(void)
{
    struct percpu *me = percpu_get();
    me->heartbeat++;
    if (!active || me->heartbeat % TIME_TICK_HZ)
        return;

    uint32_t n = percpu_count();
    struct percpu *other = percpu_by_index((me->cpu_id + 1) % n);
    if (!other || other == me || !other->online)
        return;

    uint64_t hb = other->heartbeat;
    if (hb != me->wd_seen) {
        me->wd_seen = hb;
        me->wd_strikes = 0;
        return;
    }
    if (++me->wd_strikes < WATCHDOG_SECONDS)
        return;

    smp_report_cpu(other->cpu_id, "parada com as interrupcoes desligadas");
    panic("watchdog: a CPU %u ficou %u s sem receber o timer (interrupcoes desligadas)",
          other->cpu_id, WATCHDOG_SECONDS);
}

void smp_dump(void)
{
    kprintf("  cpu  lapic  ticks      trocas   thread atual\n");
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = percpu_by_index(i);
        if (!c || !c->online)
            continue;
        kprintf("  %3u  %5u  %9lu  %7lu   %s\n", c->cpu_id, c->lapic_id, c->ticks,
                c->context_switches, c->current ? c->current->name : "-");
    }
    kprintf("  TLB shootdowns sincronos: %lu\n", tlb_sync_count);
}
