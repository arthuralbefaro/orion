#include <kernel/prof.h>
#include <kernel/cred.h>
#include <kernel/irqctx.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/percpu.h>
#include <mm/heap.h>
#include <mm/uaccess.h>

#define PROF_PER_CPU 16384

struct prof_cpu {
    struct prof_sample *buf;
    volatile uint32_t n;
    volatile uint32_t dropped;
    volatile uint32_t pending;
    uint32_t tick;
};

static struct prof_cpu cpus[MAX_CPUS];
static volatile bool running;
static uint32_t divisor = 1;
static uint32_t hz;
static uint64_t started_ms, stopped_ms;
static volatile uint32_t control_busy;

static void record(uint32_t me, struct interrupt_frame *f)
{
    struct prof_cpu *c = &cpus[me];
    if (!c->buf)
        return;
    uint32_t i = c->n;
    if (i >= PROF_PER_CPU) {
        c->dropped++;
        return;
    }
    struct thread *t = thread_current();
    struct prof_sample *s = &c->buf[i];
    s->rip = f->rip;
    s->pid = t && t->process ? t->process->pid : 0;
    s->cpu = (uint16_t)me;
    s->user = (f->cs & 3) == 3;
    s->irq_off = !(f->rflags & RFLAGS_IF);
    __atomic_store_n(&c->n, i + 1, __ATOMIC_RELEASE);
}

static struct percpu *next_cpu(uint32_t me)
{
    uint32_t n = percpu_count();
    for (uint32_t k = 1; k <= n; k++) {
        struct percpu *c = percpu_by_index((me + k) % n);
        if (c && c->online)
            return c;
    }
    return NULL;
}

void prof_tick(struct interrupt_frame *f)
{
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE))
        return;
    uint32_t me = percpu_cpu_id();
    if (me >= MAX_CPUS || ++cpus[me].tick % divisor)
        return;
    struct percpu *target = next_cpu(me);
    if (!target || target->cpu_id == me) {
        record(me, f);
        return;
    }
    if (__atomic_exchange_n(&cpus[target->cpu_id].pending, 1, __ATOMIC_ACQ_REL) == 0)
        lapic_send_nmi(target->lapic_id);
}

bool prof_nmi(struct interrupt_frame *f)
{
    uint32_t me = percpu_cpu_id();
    if (me >= MAX_CPUS || !__atomic_exchange_n(&cpus[me].pending, 0, __ATOMIC_ACQ_REL))
        return false;
    record(me, f);
    return true;
}

__attribute__((noinline)) static void prof_selftest_irqoff(uint64_t ms)
{
    uint64_t end = time_now_us() + ms * 1000;
    while (time_now_us() < end)
        pause();
}

static long prof_start(uint64_t want_hz)
{
    if (want_hz == 0 || want_hz > TIME_TICK_HZ)
        return -EINVAL;
    for (uint32_t i = 0; i < percpu_count() && i < MAX_CPUS; i++) {
        if (!cpus[i].buf)
            cpus[i].buf = kmalloc(PROF_PER_CPU * sizeof(struct prof_sample));
        if (!cpus[i].buf)
            return -ENOMEM;
    }
    __atomic_store_n(&running, false, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cpus[i].n = 0;
        cpus[i].dropped = 0;
        cpus[i].pending = 0;
        cpus[i].tick = 0;
    }
    divisor = (uint32_t)(TIME_TICK_HZ / want_hz);
    hz = TIME_TICK_HZ / divisor;
    started_ms = time_ticks();
    stopped_ms = 0;
    __atomic_store_n(&running, true, __ATOMIC_RELEASE);
    log_info("prof", "amostragem ligada: %u Hz por CPU, por NMI", hz);
    return 0;
}

static long prof_stop(void)
{
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE))
        return 0;
    __atomic_store_n(&running, false, __ATOMIC_RELEASE);
    stopped_ms = time_ticks();
    thread_sleep_ms(5);
    uint64_t total = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        total += cpus[i].n;
    log_info("prof", "amostragem desligada: %lu amostra(s) em %lu ms", total, stopped_ms - started_ms);
    return 0;
}

static long prof_read(uint64_t ubuf, uint64_t max)
{
    if (__atomic_load_n(&running, __ATOMIC_ACQUIRE))
        return -EBUSY;
    uint64_t done = 0;
    for (uint32_t i = 0; i < MAX_CPUS && done < max; i++) {
        if (!cpus[i].buf)
            continue;
        uint64_t n = cpus[i].n;
        if (n > max - done)
            n = max - done;
        if (n && copy_to_user((void *)(ubuf + done * sizeof(struct prof_sample)), cpus[i].buf,
                              n * sizeof(struct prof_sample)) < 0)
            return -EFAULT;
        done += n;
    }
    return (long)done;
}

static long prof_stats_copy(uint64_t ubuf)
{
    struct prof_stats st = { 0 };
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        st.samples += cpus[i].n;
        st.dropped += cpus[i].dropped;
    }
    uint64_t end = __atomic_load_n(&running, __ATOMIC_ACQUIRE) ? time_ticks() : stopped_ms;
    st.elapsed_ms = end > started_ms ? end - started_ms : 0;
    st.hz = hz;
    st.cpus = percpu_count();
    return copy_to_user((void *)ubuf, &st, sizeof(st)) < 0 ? -EFAULT : 0;
}

long sys_prof(uint64_t op, uint64_t arg, uint64_t ubuf, uint64_t max)
{
    if (!cred_is_root())
        return -EPERM;
    if (op == PROF_SPIN_IRQOFF) {
        if (!irqctx_strict())
            return -EPERM;
        if (arg == 0 || arg > 2000)
            return -EINVAL;
        uint64_t flags = irq_save();
        prof_selftest_irqoff(arg);
        irq_restore(flags);
        return 0;
    }
    if (__atomic_exchange_n(&control_busy, 1, __ATOMIC_ACQ_REL))
        return -EBUSY;
    long r;
    switch (op) {
    case PROF_START: r = prof_start(arg); break;
    case PROF_STOP:  r = prof_stop(); break;
    case PROF_READ:  r = prof_read(ubuf, max); break;
    case PROF_STATS: r = prof_stats_copy(ubuf); break;
    default:         r = -EINVAL; break;
    }
    __atomic_store_n(&control_busy, 0, __ATOMIC_RELEASE);
    return r;
}
