#include <kernel/smp.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/syscall.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/percpu.h>
#include <mm/vmm.h>
#include <limine.h>

static volatile uint32_t aps_online;
static bool active;

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

void smp_halt_others(void)
{
    if (active)
        lapic_send_ipi_others(VECTOR_IPI_HALT);
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
}