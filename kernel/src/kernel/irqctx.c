#include <kernel/irqctx.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/percpu.h>
#include <drivers/fwcfg.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <mm/heap.h>

#define REPORT_SLOTS 64

static uint64_t reported[REPORT_SLOTS];
static uint32_t reports;
static bool strict;
static volatile bool testing;

bool in_interrupt(void)
{
    if (!percpu_ready())
        return false;
    uint32_t depth;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(depth) : "i"(__builtin_offsetof(struct percpu, irq_depth)));
    return depth > 0;
}

void irqctx_enter(struct percpu *p, uint8_t vector, bool nmi, uint8_t *saved_vector, bool *saved_nmi)
{
    *saved_vector = p->irq_vector;
    *saved_nmi = p->in_nmi;
    p->irq_depth++;
    if (p->irq_depth > p->irq_depth_max)
        p->irq_depth_max = p->irq_depth;
    p->irq_vector = vector;
    if (nmi)
        p->in_nmi = true;
    __asm__ volatile("" ::: "memory");
}

void irqctx_leave(struct percpu *p, uint8_t saved_vector, bool saved_nmi)
{
    __asm__ volatile("" ::: "memory");
    p->in_nmi = saved_nmi;
    p->irq_vector = saved_vector;
    if (p->irq_depth == 0)
        panic("irqctx: contador de interrupcao abaixo de zero na CPU %u (vetor salvo %u)", p->cpu_id, saved_vector);
    p->irq_depth--;
}

static bool first_time(uint64_t caller)
{
    for (uint32_t i = 0; i < REPORT_SLOTS; i++) {
        uint64_t cur = __atomic_load_n(&reported[i], __ATOMIC_ACQUIRE);
        if (cur == caller)
            return false;
        if (cur != 0)
            continue;
        uint64_t expected = 0;
        if (__atomic_compare_exchange_n(&reported[i], &expected, caller, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return true;
        if (expected == caller)
            return false;
    }
    return false;
}

static void complain(const char *what, uint64_t caller, const char *how, uint32_t vector, uint32_t cpu,
                     uint32_t depth)
{
    log_error("irq", "BUG: %s dentro de interrupcao%s, chamado de %p (vetor %u, CPU %u, profundidade %u)", what,
              how, (void *)caller, vector, cpu, depth);
    if (strict && !testing)
        panic("BUG: %s dentro de interrupcao%s, chamado de %p", what, how, (void *)caller);
}

void irqctx_report(const char *what, void *caller)
{
    uint64_t c = (uint64_t)caller;
    if (!first_time(c))
        return;
    __atomic_add_fetch(&reports, 1, __ATOMIC_RELAXED);
    uint64_t flags = irq_save();
    struct percpu *p = percpu_get();
    if (p->in_nmi) {
        if (!__atomic_load_n(&p->nmi_bug_caller, __ATOMIC_ACQUIRE)) {
            p->nmi_bug_what = what;
            __atomic_store_n(&p->nmi_bug_caller, c, __ATOMIC_RELEASE);
        }
        irq_restore(flags);
        return;
    }
    uint32_t vector = p->irq_vector;
    uint32_t depth = p->irq_depth;
    uint32_t cpu = p->cpu_id;
    irq_restore(flags);
    complain(what, c, "", vector, cpu, depth);
}

void irqctx_flush_nmi(void)
{
    if (!percpu_ready())
        return;
    struct percpu *p = percpu_get();
    if (!__atomic_load_n(&p->nmi_bug_caller, __ATOMIC_ACQUIRE))
        return;
    const char *what = p->nmi_bug_what;
    uint64_t c = __atomic_exchange_n(&p->nmi_bug_caller, 0, __ATOMIC_ACQ_REL);
    if (c)
        complain(what, c, " (NMI, anotado e impresso depois)", 2, p->cpu_id, 1);
}

void irqctx_init(void)
{
    strict = fwcfg_has_file("opt/orion/strict");
    if (strict)
        log_info("irq", "modo estrito (fw_cfg opt/orion/strict): BUG dentro de interrupcao para o kernel");
}

bool irqctx_strict(void)
{
    return strict;
}

static struct mutex test_mutex = MUTEX_INIT("irqtest");
static volatile bool test_nest;
static volatile bool test_done;
static volatile uint32_t test_depth;
static volatile bool test_same;
static volatile uint64_t test_ticks;
static volatile bool nmi_armed;
static volatile bool nmi_done;
static uint32_t selftest_runs;

static __attribute__((noinline)) void test_alloc(void)
{
    void *p = kmalloc(32);
    kfree(p);
}

static void test_handler(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    struct percpu *p = percpu_get();
    test_depth = p->irq_depth;
    test_alloc();
    if (mutex_trylock(&test_mutex))
        mutex_unlock(&test_mutex);
    if (test_nest) {
        struct thread *me = p->current;
        uint64_t t0 = p->ticks;
        p->need_resched = true;
        uint64_t end = time_now_us() + 300000;
        lapic_eoi();
        sti();
        while (p->ticks < t0 + 2 && time_now_us() < end)
            pause();
        cli();
        test_ticks = p->ticks - t0;
        test_same = percpu_get() == p && p->current == me;
    }
    test_done = true;
}

bool irqctx_nmi_probe(void)
{
    if (!nmi_armed)
        return false;
    nmi_armed = false;
    MIGHT_SLEEP("teste no NMI");
    nmi_done = true;
    return true;
}

static volatile bool helper_stop;
static volatile bool helper_ran;

static void helper_spin(void *arg)
{
    (void)arg;
    helper_ran = true;
    while (!helper_stop)
        pause();
}

struct spin_result {
    uint64_t deferred;
    uint64_t switches;
    uint64_t unlock_preempts;
    bool same;
    bool ran_before;
    bool ran_at_unlock;
};

static bool spin_round(struct spin_result *r)
{
    static spinlock_t hold = SPINLOCK_INIT;
    helper_stop = false;
    helper_ran = false;
    struct thread *helper = thread_create_for_process_paused("irqtest-ajuda", helper_spin, NULL, NULL);
    if (!helper)
        return false;
    spin_lock(&hold);
    uint64_t flags = irq_save();
    struct percpu *p = percpu_get();
    irq_restore(flags);
    struct thread *me = thread_current();
    sched_set_affinity(helper, 1ull << p->cpu_id);
    thread_unblock(helper);
    uint64_t d0 = p->preempt_deferred;
    uint64_t s0 = p->context_switches;
    uint64_t t0 = p->ticks;
    p->need_resched = true;
    uint64_t end = time_now_us() + 300000;
    while (p->ticks < t0 + 3 && time_now_us() < end)
        pause();
    flags = irq_save();
    r->deferred = p->preempt_deferred - d0;
    r->switches = p->context_switches - s0;
    r->same = percpu_get() == p && thread_current() == me;
    r->ran_before = helper_ran;
    p->need_resched = true;
    irq_restore(flags);
    uint64_t u0 = p->preempt_unlock;
    spin_unlock(&hold);
    r->ran_at_unlock = helper_ran;
    r->unlock_preempts = p->preempt_unlock - u0;
    helper_stop = true;
    return r->same;
}

static bool fire(int vector, bool nest)
{
    test_nest = nest;
    test_done = false;
    uint64_t flags = irq_save();
    lapic_send_ipi(lapic_id(), (uint8_t)vector);
    irq_restore(flags);
    uint64_t end = time_now_us() + 1000000;
    while (!test_done && time_now_us() < end)
        pause();
    return test_done;
}

static bool nmi_round(void)
{
    nmi_done = false;
    uint64_t flags = irq_save();
    struct percpu *p = percpu_get();
    nmi_armed = true;
    lapic_send_nmi(lapic_id());
    uint64_t end = time_now_us() + 1000000;
    while (!nmi_done && time_now_us() < end)
        pause();
    bool noted = nmi_done && (__atomic_load_n(&p->nmi_bug_caller, __ATOMIC_ACQUIRE) != 0 || selftest_runs > 0);
    irq_restore(flags);
    end = time_now_us() + 1000000;
    while (__atomic_load_n(&p->nmi_bug_caller, __ATOMIC_ACQUIRE) && time_now_us() < end)
        pause();
    return noted && !__atomic_load_n(&p->nmi_bug_caller, __ATOMIC_ACQUIRE);
}

int irqctx_selftest(void)
{
    if (!strict)
        return -EPERM;
    int vector = interrupts_alloc_vector(test_handler, NULL, "irqtest");
    if (vector < 0)
        return -1;
    testing = true;
    uint32_t expect = selftest_runs == 0 ? 1 : 0;
    uint32_t base = __atomic_load_n(&reports, __ATOMIC_RELAXED);

    bool fired = fire(vector, false) && fire(vector, false);
    bool depth_ok = test_depth == 1;
    uint32_t in_irq = __atomic_load_n(&reports, __ATOMIC_RELAXED) - base;

    static spinlock_t lock = SPINLOCK_INIT;
    uint64_t flags = spin_lock_irqsave(&lock);
    void *x = kmalloc(16);
    spin_unlock_irqrestore(&lock, flags);
    kfree(x);
    uint32_t outside = __atomic_load_n(&reports, __ATOMIC_RELAXED) - base - in_irq;

    percpu_get()->irq_depth_max = 0;
    bool nested = fire(vector, true);
    uint32_t max_depth = 0;
    for (uint32_t i = 0; i < percpu_count(); i++) {
        struct percpu *c = percpu_by_index(i);
        if (c && c->irq_depth_max > max_depth)
            max_depth = c->irq_depth_max;
    }

    struct spin_result sr = { 0 };
    bool held = spin_round(&sr);

    uint32_t before_nmi = __atomic_load_n(&reports, __ATOMIC_RELAXED);
    bool nmi_ok = nmi_round();
    uint32_t nmi_reports = __atomic_load_n(&reports, __ATOMIC_RELAXED) - before_nmi;

    testing = false;
    interrupts_unregister((uint8_t)vector);
    selftest_runs++;

    bool ok = fired && depth_ok && in_irq == expect && outside == 0 && nested && max_depth >= 2 &&
              test_ticks >= 2 && test_same && nmi_ok && nmi_reports == expect && held && sr.deferred >= 1 &&
              sr.switches == 0 && !sr.ran_before && sr.ran_at_unlock && sr.unlock_preempts >= 1;
    log_info("irq",
             "irqtest: kmalloc na IRQ avisou %u vez(es) em 2 chamadas (profundidade %u); trylock nao "
             "avisou; kmalloc com trava de spin fora de IRQ avisou %u vez(es)",
             in_irq, test_depth, outside);
    log_info("irq", "irqtest: IRQ aninhada: profundidade maxima %u, %lu tick(s) do timer dentro da IRQ, "
             "mesma thread no fim: %s; NMI: %u aviso(s), anotado e impresso depois: %s",
             max_depth, test_ticks, test_same ? "sim" : "nao", nmi_reports, nmi_ok ? "sim" : "nao");
    log_info("irq", "irqtest: trava de spin com interrupcoes ligadas: %lu troca(s) adiada(s), %lu troca(s) de thread "
             "na CPU enquanto a trava estava presa, mesma thread e CPU: %s",
             sr.deferred, sr.switches, held ? "sim" : "nao");
    log_info("irq", "irqtest: spin_unlock com troca adiada: a thread da mesma CPU rodou antes: %s, rodou antes do "
             "spin_unlock voltar: %s (%lu troca(s) na liberacao)",
             sr.ran_before ? "sim" : "nao", sr.ran_at_unlock ? "sim" : "nao", sr.unlock_preempts);
    return ok ? 0 : -EIO;
}
