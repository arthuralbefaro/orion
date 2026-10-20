#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/wait.h>
#include <kernel/syscall.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <kernel/irqctx.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/fpu.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/interrupts.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

#define NICE_0_WEIGHT  1024
#define SLICE_US       ((uint64_t)SCHED_TIME_SLICE * 1000000ull / TIME_TICK_HZ)
#define WAKEUP_GRAN_US 1000
#define BALANCE_TICKS  4

static spinlock_t runq_lock = SPINLOCK_INIT;
static struct thread *all_threads;
static struct thread *sleeping;
static uint32_t next_tid = 1;
static bool enabled;

static const uint32_t prio_weight[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
    9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
    1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
    110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static const char *const state_name[] = {
    [THREAD_READY]    = "ready",
    [THREAD_RUNNING]  = "running",
    [THREAD_SLEEPING] = "sleeping",
    [THREAD_BLOCKED]  = "blocked",
    [THREAD_ZOMBIE]   = "zombie",
};

uint32_t sched_weight(int nice)
{
    if (nice < NICE_MIN)
        nice = NICE_MIN;
    if (nice > NICE_MAX)
        nice = NICE_MAX;
    return prio_weight[nice - NICE_MIN];
}

static uint32_t weight_of(const struct thread *t)
{
    return sched_weight(t->nice);
}

static bool vr_before(uint64_t a, uint64_t b)
{
    return (int64_t)(a - b) < 0;
}

static struct percpu *cpu_at(uint32_t i)
{
    struct percpu *c = percpu_by_index(i);
    return c && c->online ? c : NULL;
}

static bool allowed_on(const struct thread *t, const struct percpu *c)
{
    return c && c->online && c->cpu_id < 64 && (t->allowed >> c->cpu_id) & 1;
}

static bool cpu_is_free(const struct percpu *c)
{
    return c->current && c->current->is_idle && c->nr_ready == 0;
}

static uint64_t cpu_load(const struct percpu *c)
{
    uint64_t l = c->rq_load;
    if (c->current && !c->current->is_idle)
        l += weight_of(c->current);
    return l;
}

static void rq_insert(struct percpu *c, struct thread *t)
{
    ASSERT_HELD(&runq_lock);
    if (t->state != THREAD_READY || t->on_runq)
        panic("sched: thread %s (tid %u) entrando na fila em estado invalido (%s, na fila: %d)",
              t->name, t->tid, state_name[t->state], t->on_runq);
    t->on_runq = true;
    t->cpu = c->cpu_id;
    t->next = NULL;
    if (c->rq_tail)
        c->rq_tail->next = t;
    else
        c->rq_head = t;
    c->rq_tail = t;
    c->nr_ready++;
    c->rq_load += weight_of(t);
}

static void rq_remove(struct percpu *c, struct thread *t)
{
    struct thread *prev = NULL;
    for (struct thread *x = c->rq_head; x; prev = x, x = x->next) {
        if (x != t)
            continue;
        if (prev)
            prev->next = t->next;
        else
            c->rq_head = t->next;
        if (c->rq_tail == t)
            c->rq_tail = prev;
        t->next = NULL;
        t->on_runq = false;
        c->nr_ready--;
        c->rq_load -= weight_of(t);
        return;
    }
    panic("sched: thread %s (tid %u) nao esta na fila da CPU %u", t->name, t->tid, c->cpu_id);
}

static void migrate_vruntime(struct thread *t, const struct percpu *from, const struct percpu *to)
{
    if (from && from != to)
        t->vruntime = t->vruntime - from->min_vruntime + to->min_vruntime;
}

static struct percpu *select_cpu(struct thread *t)
{
    struct percpu *prev = cpu_at(t->cpu);
    if (allowed_on(t, prev) && cpu_is_free(prev))
        return prev;
    struct percpu *best = NULL;
    uint64_t best_load = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = cpu_at(i);
        if (!allowed_on(t, c))
            continue;
        if (cpu_is_free(c))
            return c;
        uint64_t l = cpu_load(c);
        if (!best || l < best_load || (l == best_load && c == prev)) {
            best = c;
            best_load = l;
        }
    }
    if (best)
        return best;
    return percpu_get();
}

static uint64_t running_vruntime(struct percpu *c)
{
    struct thread *cur = c->current;
    uint64_t now = time_now_us();
    uint64_t vr = cur->vruntime;
    if (now > c->switch_us)
        vr += (now - c->switch_us) * NICE_0_WEIGHT / weight_of(cur);
    return vr;
}

static void kick(struct percpu *c, struct thread *t)
{
    struct thread *cur = c->current;
    bool want = !cur || cur->is_idle ||
                (int64_t)(running_vruntime(c) - t->vruntime) > WAKEUP_GRAN_US;
    if (!want)
        return;
    c->need_resched = true;
    if (c != percpu_get()) {
        c->resched_ipis++;
        lapic_send_ipi(c->lapic_id, VECTOR_IPI_RESCHED);
    }
}

static void place_ready(struct thread *t)
{
    ASSERT_HELD(&runq_lock);
    struct percpu *from = cpu_at(t->cpu);
    struct percpu *to = select_cpu(t);
    if (t->vr_fresh) {
        t->vruntime = to->min_vruntime;
        t->vr_fresh = false;
    } else {
        migrate_vruntime(t, from, to);
        uint64_t floor = to->min_vruntime - SLICE_US;
        if (vr_before(t->vruntime, floor))
            t->vruntime = floor;
    }
    if (from != to && from)
        to->migrations++;
    rq_insert(to, t);
    kick(to, t);
}

static struct thread *rq_pick(struct percpu *c)
{
    struct thread *best = NULL;
    for (struct thread *x = c->rq_head; x; x = x->next)
        if (!best || vr_before(x->vruntime, best->vruntime))
            best = x;
    if (best) {
        rq_remove(c, best);
        if (vr_before(c->min_vruntime, best->vruntime))
            c->min_vruntime = best->vruntime;
    }
    return best;
}

static struct thread *movable_from(struct percpu *src, struct percpu *dst, uint64_t below)
{
    struct thread *pick = NULL;
    for (struct thread *x = src->rq_head; x; x = x->next) {
        if (!allowed_on(x, dst))
            continue;
        if (below && weight_of(x) >= below)
            continue;
        pick = x;
    }
    return pick;
}

static void move_thread(struct thread *t, struct percpu *src, struct percpu *dst)
{
    rq_remove(src, t);
    migrate_vruntime(t, src, dst);
    dst->migrations++;
}

static struct thread *steal(struct percpu *cpu)
{
    struct percpu *src = NULL;
    struct thread *t = NULL;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = cpu_at(i);
        if (!c || c == cpu || !c->nr_ready)
            continue;
        if (src && c->nr_ready <= src->nr_ready)
            continue;
        struct thread *x = movable_from(c, cpu, 0);
        if (x) {
            src = c;
            t = x;
        }
    }
    if (!t)
        return NULL;
    move_thread(t, src, cpu);
    t->cpu = cpu->cpu_id;
    cpu->steals++;
    if (vr_before(cpu->min_vruntime, t->vruntime))
        cpu->min_vruntime = t->vruntime;
    return t;
}

static void balance(struct percpu *cpu)
{
    uint64_t mine = cpu_load(cpu);
    struct percpu *busiest = NULL;
    uint64_t most = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = cpu_at(i);
        if (!c || c == cpu || !c->nr_ready)
            continue;
        uint64_t l = cpu_load(c);
        if (l > most) {
            most = l;
            busiest = c;
        }
    }
    if (!busiest || most <= mine)
        return;
    struct thread *t = movable_from(busiest, cpu, most - mine);
    if (!t)
        return;
    move_thread(t, busiest, cpu);
    rq_insert(cpu, t);
    cpu->balance_moves++;
    if (cpu->current && cpu->current->is_idle)
        cpu->need_resched = true;
}

static bool work_elsewhere(struct percpu *cpu)
{
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = cpu_at(i);
        if (c && c != cpu && c->nr_ready && movable_from(c, cpu, 0))
            return true;
    }
    return false;
}

static void sleeping_remove(struct thread *t)
{
    if (!t->on_sleep_list)
        return;
    struct thread **pp = &sleeping;
    while (*pp && *pp != t)
        pp = &(*pp)->next;
    if (*pp)
        *pp = t->next;
    t->on_sleep_list = false;
    t->next = NULL;
}

static struct thread *detach_zombie(struct percpu *cpu)
{
    struct thread *z = cpu->zombie;
    if (!z)
        return NULL;
    cpu->zombie = NULL;

    struct thread **pp = &all_threads;
    while (*pp && *pp != z)
        pp = &(*pp)->all_next;
    if (*pp)
        *pp = z->all_next;
    return z;
}

static void free_thread(struct thread *z)
{
    if (!z)
        return;
    if (z->process && __atomic_sub_fetch(&z->process->thread_refs, 1, __ATOMIC_ACQ_REL) == 0)
        process_thread_reaped(z->process);
    kfree(z->stack);
    kfree(z->fpu);
    kfree(z);
}

static struct thread *thread_alloc(const char *name)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
    strlcpy(t->name, name, sizeof(t->name));
    t->slice_left = SCHED_TIME_SLICE;
    t->allowed = ~0ull;
    t->vr_fresh = true;

    uint64_t flags = spin_lock_irqsave(&runq_lock);
    t->all_next = all_threads;
    all_threads = t;
    spin_unlock_irqrestore(&runq_lock, flags);
    return t;
}

static struct thread *thread_create_common(const char *name, thread_entry_t entry, void *arg,
                                           struct process *process, bool start)
{
    struct thread *t = thread_alloc(name);
    if (!t)
        return NULL;

    t->stack_size = THREAD_STACK_SIZE;
    t->stack = kmalloc(t->stack_size);
    if (process)
        t->fpu = kmalloc(FPU_STATE_SIZE);
    if (!t->stack || (process && !t->fpu)) {
        kfree(t->stack);
        kfree(t->fpu);
        uint64_t flags = spin_lock_irqsave(&runq_lock);
        struct thread **pp = &all_threads;
        while (*pp && *pp != t)
            pp = &(*pp)->all_next;
        if (*pp)
            *pp = t->all_next;
        spin_unlock_irqrestore(&runq_lock, flags);
        kfree(t);
        return NULL;
    }
    *(uint64_t *)t->stack = THREAD_STACK_CANARY;

    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack + t->stack_size);
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0x2;
    *--sp = 0;
    *--sp = 0;
    *--sp = (uint64_t)entry;
    *--sp = (uint64_t)arg;
    *--sp = 0;
    *--sp = 0;
    t->rsp = (uint64_t)sp;
    t->kernel_stack_top = (uint64_t)t->stack + t->stack_size;
    t->process = process;
    if (t->fpu)
        fpu_init_state(t->fpu);
    struct thread *cur = enabled ? thread_current() : NULL;
    if (process && cur && cur->process) {
        t->nice = cur->nice;
        t->allowed = cur->allowed;
    }

    uint64_t flags = spin_lock_irqsave(&runq_lock);
    if (start) {
        t->state = THREAD_READY;
        place_ready(t);
    } else {
        t->state = THREAD_BLOCKED;
    }
    spin_unlock_irqrestore(&runq_lock, flags);
    return t;
}

struct thread *thread_create_for_process(const char *name, thread_entry_t entry, void *arg,
                                         struct process *process)
{
    return thread_create_common(name, entry, arg, process, true);
}

struct thread *thread_create_for_process_paused(const char *name, thread_entry_t entry, void *arg,
                                                struct process *process)
{
    return thread_create_common(name, entry, arg, process, false);
}

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg)
{
    return thread_create_for_process(name, entry, arg, NULL);
}

static bool idle_has_work(struct percpu *cpu)
{
    return cpu->need_resched || cpu->nr_ready;
}

NORETURN void sched_idle(void)
{
    for (;;) {
        cli();
        struct percpu *cpu = percpu_get();
        if (enabled && idle_has_work(cpu))
            schedule();
        else
            sti_hlt();
    }
}

static void idle_loop(void *arg)
{
    (void)arg;
    sched_idle();
}

static void ipi_resched(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
}

void sched_init(void)
{
    struct percpu *cpu = percpu_get();

    struct thread *main_thread = thread_alloc("main");
    main_thread->state = THREAD_RUNNING;
    main_thread->cpu = 0;
    main_thread->vr_fresh = false;
    cpu->current = main_thread;

    struct thread *idle = thread_create_for_process_paused("idle0", idle_loop, NULL, NULL);
    if (!idle)
        panic("sched: sem memoria para a idle0");
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    idle->state = THREAD_READY;
    idle->is_idle = true;
    cpu->idle = idle;
    spin_unlock_irqrestore(&runq_lock, flags);

    interrupts_register(VECTOR_IPI_RESCHED, ipi_resched, NULL);
    enabled = true;
    log_info("sched", "escalonador ativo: fila por CPU, fatia de %u ms, nice -20..19, main tid=%u, idle tid=%u",
             SCHED_TIME_SLICE * 1000 / TIME_TICK_HZ, main_thread->tid, idle->tid);
}

void sched_init_ap(void)
{
    struct percpu *cpu = percpu_get();
    char name[THREAD_NAME_MAX];
    ksnprintf(name, sizeof(name), "idle%u", cpu->cpu_id);

    struct thread *idle = thread_alloc(name);
    idle->state = THREAD_RUNNING;
    idle->is_idle = true;
    idle->cpu = cpu->cpu_id;
    cpu->idle = idle;
    cpu->current = idle;
}

bool sched_enabled(void)
{
    return enabled;
}

struct thread *thread_current(void)
{
    struct thread *t;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(t) : "i"(__builtin_offsetof(struct percpu, current)));
    return t;
}

static void account(struct percpu *cpu, struct thread *prev)
{
    uint64_t now_us = time_now_us();
    if (now_us > cpu->switch_us) {
        uint64_t delta = now_us - cpu->switch_us;
        prev->run_us += delta;
        if (!prev->is_idle)
            prev->vruntime += delta * NICE_0_WEIGHT / weight_of(prev);
    }
    cpu->switch_us = now_us;
}

static void switch_to(struct percpu *cpu, struct thread *prev, struct thread *next)
{
    next->state = THREAD_RUNNING;
    next->slice_left = SCHED_TIME_SLICE;
    next->cpu = cpu->cpu_id;
    cpu->current = next;
    cpu->need_resched = false;
    cpu->context_switches++;

    if (prev->stack && *(uint64_t *)prev->stack != THREAD_STACK_CANARY)
        panic("sched: a pilha do kernel da thread %s (tid %u) estourou", prev->name, prev->tid);
    uint64_t cr3 = next->process ? next->process->pml4_phys : vmm_kernel_pml4();
    if (!cr3)
        panic("sched: thread %s (tid %u) sem espaco de enderecamento (processo ja colhido?)",
              next->name, next->tid);
    if ((read_cr3() & PTE_ADDR_MASK) != cr3)
        write_cr3(cr3);
    gdt_set_kernel_stack(next->kernel_stack_top);
    percpu_set_kernel_rsp(next->kernel_stack_top);
    if (next->fs_base != prev->fs_base)
        wrmsr(MSR_IA32_FS_BASE, next->fs_base);
    if (prev->fpu)
        fpu_save(prev->fpu);
    if (next->fpu)
        fpu_restore(next->fpu);

    context_switch(&prev->rsp, next->rsp);
}

void sched_post_switch(void)
{
    struct thread *z = detach_zombie(percpu_get());
    spin_unlock(&runq_lock);
    free_thread(z);
}

static void schedule_locked(void)
{
    MIGHT_SLEEP("troca de thread");
    struct percpu *cpu = percpu_get();
    struct thread *prev = cpu->current;
    account(cpu, prev);

    if (prev->state == THREAD_RUNNING && !prev->is_idle) {
        prev->state = THREAD_READY;
        if (allowed_on(prev, cpu))
            rq_insert(cpu, prev);
        else
            place_ready(prev);
    } else if (prev->is_idle) {
        prev->state = THREAD_READY;
    } else if (prev->state == THREAD_ZOMBIE) {
        cpu->zombie = prev;
    }

    struct thread *next = rq_pick(cpu);
    if (!next)
        next = steal(cpu);
    if (!next)
        next = cpu->idle;

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        prev->slice_left = SCHED_TIME_SLICE;
        cpu->need_resched = false;
        spin_unlock(&runq_lock);
        return;
    }

    switch_to(cpu, prev, next);
    sched_post_switch();
}

void schedule(void)
{
    ASSERT(!interrupts_enabled());
    spin_lock(&runq_lock);
    schedule_locked();
}

void sched_runq_lock(void)
{
    spin_lock(&runq_lock);
}

void sched_runq_unlock(void)
{
    spin_unlock(&runq_lock);
}

bool sched_runq_held(void)
{
    return spin_held(&runq_lock);
}

uint64_t sched_deadline(uint64_t ms)
{
    return time_ticks() + DIV_ROUND_UP(ms * TIME_TICK_HZ, 1000);
}

void sched_block_switch(uint64_t wake_at)
{
    ASSERT_HELD(&runq_lock);
    ASSERT(!interrupts_enabled());
    struct thread *t = percpu_get()->current;
    if (wake_at) {
        t->wake_at = wake_at;
        t->state = THREAD_SLEEPING;
        t->next = sleeping;
        sleeping = t;
        t->on_sleep_list = true;
    } else {
        t->state = THREAD_BLOCKED;
    }
    schedule_locked();
}

bool sched_make_ready(struct thread *t)
{
    ASSERT_HELD(&runq_lock);
    if (t->state != THREAD_BLOCKED && t->state != THREAD_SLEEPING)
        return false;
    sleeping_remove(t);
    t->state = THREAD_READY;
    place_ready(t);
    return true;
}

void thread_yield(void)
{
    MIGHT_SLEEP("thread_yield");
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void thread_sleep_ms(uint64_t ms)
{
    MIGHT_SLEEP("thread_sleep_ms");
    uint64_t flags = irq_save();
    spin_lock(&runq_lock);
    sched_block_switch(sched_deadline(ms));
    irq_restore(flags);
}

void thread_block(void)
{
    MIGHT_SLEEP("thread_block");
    uint64_t flags = irq_save();
    spin_lock(&runq_lock);
    percpu_get()->current->state = THREAD_BLOCKED;
    schedule_locked();
    irq_restore(flags);
}

void thread_unblock(struct thread *t)
{
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        place_ready(t);
    }
    spin_unlock_irqrestore(&runq_lock, flags);
}

NORETURN void thread_exit(void)
{
    cli();
    struct thread *t = thread_current();
    if (t->is_idle || !t->stack)
        panic("thread_exit chamado pela thread %s", t->name);
    spin_lock(&runq_lock);
    if (t->waits)
        panic("thread_exit: %s (tid %u) saiu ainda registrada numa fila de espera", t->name, t->tid);
    if (t->on_sleep_list || t->on_runq)
        panic("thread_exit: %s (tid %u) saiu ainda numa fila do escalonador", t->name, t->tid);
    t->state = THREAD_ZOMBIE;
    schedule_locked();
    panic("thread zombie voltou a executar");
}

uint64_t sched_online_mask(void)
{
    uint64_t m = 0;
    for (uint32_t i = 0; i < MAX_CPUS && i < 64; i++)
        if (cpu_at(i))
            m |= 1ull << i;
    return m;
}

long sched_set_affinity(struct thread *t, uint64_t mask)
{
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    mask &= sched_online_mask();
    if (!mask) {
        spin_unlock_irqrestore(&runq_lock, flags);
        return -EINVAL;
    }
    t->allowed = mask;
    bool move_self = false;
    if (t->on_runq) {
        struct percpu *c = cpu_at(t->cpu);
        if (c && !allowed_on(t, c)) {
            rq_remove(c, t);
            place_ready(t);
        }
    } else if (t->state == THREAD_RUNNING) {
        struct percpu *c = cpu_at(t->cpu);
        if (c && !allowed_on(t, c)) {
            c->need_resched = true;
            if (t == thread_current())
                move_self = true;
            else if (c != percpu_get())
                lapic_send_ipi(c->lapic_id, VECTOR_IPI_RESCHED);
        }
    }
    spin_unlock_irqrestore(&runq_lock, flags);
    if (move_self)
        thread_yield();
    return 0;
}

uint64_t sched_get_affinity(struct thread *t)
{
    return t->allowed & sched_online_mask();
}

void sched_set_nice(struct thread *t, int nice)
{
    if (nice < NICE_MIN)
        nice = NICE_MIN;
    if (nice > NICE_MAX)
        nice = NICE_MAX;
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    if (t->on_runq) {
        struct percpu *c = cpu_at(t->cpu);
        c->rq_load -= weight_of(t);
        t->nice = nice;
        c->rq_load += weight_of(t);
    } else {
        t->nice = nice;
    }
    spin_unlock_irqrestore(&runq_lock, flags);
}

bool sched_cpu_stat(uint32_t index, struct sched_cpu_stat *out)
{
    struct percpu *c = cpu_at(index);
    if (!c)
        return false;
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    out->cpu = c->cpu_id;
    out->nr_ready = c->nr_ready;
    out->load = cpu_load(c);
    out->min_vruntime = c->min_vruntime;
    out->context_switches = c->context_switches;
    out->migrations = c->migrations;
    out->steals = c->steals;
    out->balance_moves = c->balance_moves;
    out->resched_ipis = c->resched_ipis;
    out->busy = c->current && !c->current->is_idle;
    spin_unlock_irqrestore(&runq_lock, flags);
    return true;
}

void thread_cpu_times(struct thread *t, uint64_t *user_us, uint64_t *sys_us)
{
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    uint64_t run = t->run_us;
    if (t->state == THREAD_RUNNING) {
        struct percpu *c = percpu_by_index(t->cpu);
        uint64_t now = time_now_us();
        if (c && c->current == t && now > c->switch_us)
            run += now - c->switch_us;
    }
    if (run < t->prev_utime + t->prev_stime)
        run = t->prev_utime + t->prev_stime;
    uint64_t samples = t->utime + t->stime;
    uint64_t ut = t->utime;
    while (samples > 0xFFFFFFFFull) {
        samples >>= 1;
        ut >>= 1;
    }
    uint64_t user = samples ? run / samples * ut + run % samples * ut / samples : run;
    if (user > run)
        user = run;
    uint64_t sys = run - user;
    if (sys < t->prev_stime)
        sys = t->prev_stime;
    user = run - sys;
    if (user < t->prev_utime) {
        user = t->prev_utime;
        sys = run - user;
    }
    t->prev_utime = user;
    t->prev_stime = sys;
    spin_unlock_irqrestore(&runq_lock, flags);
    *user_us = user;
    *sys_us = sys;
}

void sched_tick(uint64_t now, bool user)
{
    if (!enabled)
        return;

    spin_lock(&runq_lock);
    struct percpu *cpu = percpu_get();
    struct thread *cur = cpu->current;
    cpu->ticks++;
    cur->total_ticks++;
    if (user) {
        cur->utime++;
        cpu->user_ticks++;
    } else if (!cur->is_idle) {
        cur->stime++;
        cpu->sys_ticks++;
    } else {
        cpu->idle_ticks++;
    }

    if (cpu->cpu_id == 0) {
        struct thread **pp = &sleeping;
        while (*pp) {
            struct thread *t = *pp;
            if (t->wake_at <= now) {
                *pp = t->next;
                t->on_sleep_list = false;
                t->next = NULL;
                wait_unlink(t);
                t->state = THREAD_READY;
                place_ready(t);
            } else {
                pp = &t->next;
            }
        }
    }

    if (cpu->ticks % BALANCE_TICKS == cpu->cpu_id % BALANCE_TICKS)
        balance(cpu);

    if (cur->is_idle) {
        if (cpu->nr_ready || work_elsewhere(cpu))
            cpu->need_resched = true;
    } else if (cur->slice_left == 0 || --cur->slice_left == 0) {
        cpu->need_resched = true;
    }
    spin_unlock(&runq_lock);
}

void sched_check_preempt(void)
{
    if (!enabled || !percpu_ready())
        return;
    struct percpu *cpu = percpu_get();
    if (!cpu->need_resched)
        return;
    if (cpu->current && __atomic_load_n(&cpu->current->spin_count, __ATOMIC_ACQUIRE)) {
        cpu->preempt_deferred++;
        return;
    }
    schedule();
}

void sched_dump(void)
{
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    kprintf("  tid  estado    cpu  nice  ticks     nome\n");
    for (struct thread *t = all_threads; t; t = t->all_next)
        kprintf("  %3u  %-8s  %3u  %4d  %8lu  %s%s\n", t->tid, state_name[t->state], t->cpu, t->nice,
                t->total_ticks, t->name, t->process ? " [user]" : "");
    spin_unlock_irqrestore(&runq_lock, flags);
}
