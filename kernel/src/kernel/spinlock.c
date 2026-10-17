#include <kernel/spinlock.h>
#include <kernel/log.h>
#include <kernel/smp.h>
#include <kernel/sched.h>
#include <arch/x86_64/percpu.h>

#define SPIN_TIMEOUT_SECONDS    10
#define SPIN_TIMEOUT_PREEMPTIBLE 60

static uint64_t tsc_per_second;

static struct thread *current_or_null(void)
{
    return percpu_ready() ? thread_current() : NULL;
}

void spin_set_timeout(uint64_t tsc_hz)
{
    tsc_per_second = tsc_hz;
}

void spin_set_owner(spinlock_t *l, uint64_t pc)
{
    l->owner_cpu = percpu_cpu_id() + 1;
    l->owner_pc = pc;
    l->owner_thread = current_or_null();
}

bool spin_held(spinlock_t *l)
{
    return l->locked && l->owner_cpu == percpu_cpu_id() + 1;
}

static NORETURN void spin_deadlock(spinlock_t *l, bool recursive, uint32_t seconds)
{
    uint32_t me = percpu_cpu_id();
    struct thread *owner = l->owner_thread;
    uint32_t owner_cpu = l->owner_cpu;
    uint64_t pc = l->owner_pc;

    if (recursive)
        panic("deadlock: a thread %s tentou pegar de novo a trava %p, que ela mesma segura (pega em %p)",
              owner ? owner->name : "?", (void *)l, (void *)pc);

    if (owner && owner->state == THREAD_RUNNING && owner->cpu != me)
        smp_report_cpu(owner->cpu, "roda a thread que segura a trava");
    else if (!owner && owner_cpu && owner_cpu - 1 != me)
        smp_report_cpu(owner_cpu - 1, "segura a trava");

    panic("deadlock provavel: a CPU %u esperou mais de %u s pela trava %p, pega em %p pela "
          "thread %s (tid %u, estado %d, cpu %d)",
          me, seconds, (void *)l, (void *)pc, owner ? owner->name : "?", owner ? owner->tid : 0,
          owner ? (int)owner->state : -1, (int)owner_cpu - 1);
}

void spin_lock_slow(spinlock_t *l)
{
    struct thread *me = current_or_null();
    bool can_yield = me && !me->is_idle && sched_enabled() && interrupts_enabled();
    uint32_t seconds = can_yield ? SPIN_TIMEOUT_PREEMPTIBLE : SPIN_TIMEOUT_SECONDS;
    uint64_t limit = tsc_per_second * seconds;
    uint64_t start = rdtsc();
    uint32_t n = 0;

    if (me && l->owner_thread == me && l->locked)
        spin_deadlock(l, true, 0);

    for (;;) {
        while (l->locked) {
            if (can_yield) {
                thread_yield();
                n += 0x400;
            } else {
                pause();
                n++;
            }
            if (n >= 0x4000) {
                n = 0;
                if (limit && rdtsc() - start > limit)
                    spin_deadlock(l, false, seconds);
            }
        }
        if (__sync_lock_test_and_set(&l->locked, 1) == 0)
            return;
    }
}
