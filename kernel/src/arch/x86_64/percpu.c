#include <arch/x86_64/percpu.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>

static struct percpu cpus[MAX_CPUS];
static uint32_t cpu_count;
static bool ready;

static void percpu_load(struct percpu *p)
{
    p->self = p;
    p->online = true;
    wrmsr(MSR_IA32_GS_BASE, (uint64_t)p);
    wrmsr(MSR_IA32_KERNEL_GS_BASE, 0);
}

void percpu_init_bsp(uint32_t lapic_id)
{
    cpus[0].cpu_id = 0;
    cpus[0].lapic_id = lapic_id;
    percpu_load(&cpus[0]);
    cpu_count = 1;
    ready = true;
    log_info("percpu", "cpu0 em %p (GS_BASE), lapic %u", (void *)&cpus[0], lapic_id);
}

void percpu_init_ap(uint32_t index, uint32_t lapic_id)
{
    cpus[index].cpu_id = index;
    cpus[index].lapic_id = lapic_id;
    percpu_load(&cpus[index]);
    __atomic_fetch_add(&cpu_count, 1, __ATOMIC_SEQ_CST);
}

bool percpu_ready(void)
{
    return ready;
}

struct percpu *percpu_get(void)
{
    struct percpu *p;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(p) : "i"(__builtin_offsetof(struct percpu, self)));
    return p;
}

struct percpu *percpu_by_index(uint32_t index)
{
    return index < MAX_CPUS ? &cpus[index] : NULL;
}

uint32_t percpu_cpu_id(void)
{
    if (!ready)
        return 0;
    uint32_t id;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(id) : "i"(__builtin_offsetof(struct percpu, cpu_id)));
    return id;
}

uint32_t percpu_count(void)
{
    return cpu_count;
}

void percpu_set_kernel_rsp(uint64_t rsp)
{
    __asm__ volatile("movq %0, %%gs:%c1" : : "r"(rsp), "i"(__builtin_offsetof(struct percpu, kernel_rsp)) : "memory");
}
