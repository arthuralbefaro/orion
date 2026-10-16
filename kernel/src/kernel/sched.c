#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/wait.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/percpu.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

static spinlock_t runq_lock = SPINLOCK_INIT;
static struct thread *all_threads;
static struct thread *ready_head, *ready_tail;
static struct thread *sleeping;
static uint32_t next_tid = 1;
static bool enabled;

static const char *const state_name[] = {
    [THREAD_READY]    = "ready",
    [THREAD_RUNNING]  = "running",
    [THREAD_SLEEPING] = "sleeping",
    [THREAD_BLOCKED]  = "blocked",
    [THREAD_ZOMBIE]   = "zombie",
};

static void ready_enqueue(struct thread *t)
{
    ASSERT_HELD(&runq_lock);
    if (t->state != THREAD_READY || t->on_runq)
        panic("sched: thread %s (tid %u) entrando na fila em estado invalido (%s, na fila: %d)",
              t->name, t->tid, state_name[t->state], t->on_runq);
    t->on_runq = true;
    t->next = NULL;
    if (ready_tail)
        ready_tail->next = t;
    else
        ready_head = t;
    ready_tail = t;
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

static struct thread *ready_dequeue(void)
{
    struct thread *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head)
            ready_tail = NULL;
        t->next = NULL;
        t->on_runq = false;
    }
    return t;
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
    if (z->process)
        process_thread_reaped(z->process);
    kfree(z->stack);
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

    uint64_t flags = spin_lock_irqsave(&runq_lock);
    t->all_next = all_threads;
    all_threads = t;
    spin_unlock_irqrestore(&runq_lock, flags);
    return t;
}

struct thread *thread_create_for_process(const char *name, thread_entry_t entry, void *arg,
                                         struct process *process)
{
    struct thread *t = thread_alloc(name);
    if (!t)
        return NULL;

    t->stack_size = THREAD_STACK_SIZE;
    t->stack = kmalloc(t->stack_size);
    if (!t->stack) {
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

    uint64_t flags = spin_lock_irqsave(&runq_lock);
    t->state = THREAD_READY;
    ready_enqueue(t);
    spin_unlock_irqrestore(&runq_lock, flags);
    return t;
}

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg)
{
    return thread_create_for_process(name, entry, arg, NULL);
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        sti();
        hlt();
    }
}

void sched_init(void)
{
    struct percpu *cpu = percpu_get();

    struct thread *main_thread = thread_alloc("main");
    main_thread->state = THREAD_RUNNING;
    main_thread->cpu = 0;
    cpu->current = main_thread;

    struct thread *idle = thread_create("idle0", idle_loop, NULL);
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    ready_dequeue();
    idle->state = THREAD_READY;
    idle->is_idle = true;
    cpu->idle = idle;
    spin_unlock_irqrestore(&runq_lock, flags);

    enabled = true;
    log_info("sched", "escalonador ativo: fatia de %u ms, main tid=%u, idle tid=%u",
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
    return percpu_get()->current;
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
    struct percpu *cpu = percpu_get();
    struct thread *prev = cpu->current;
    struct thread *next = ready_dequeue();

    if (!next) {
        if (prev->state == THREAD_RUNNING) {
            spin_unlock(&runq_lock);
            return;
        }
        next = cpu->idle;
    }

    if (prev->state == THREAD_RUNNING && !prev->is_idle) {
        prev->state = THREAD_READY;
        ready_enqueue(prev);
    } else if (prev->is_idle) {
        prev->state = THREAD_READY;
    } else if (prev->state == THREAD_ZOMBIE) {
        cpu->zombie = prev;
    }

    if (next == prev) {
        prev->state = THREAD_RUNNING;
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
    ready_enqueue(t);
    percpu_get()->need_resched = true;
    return true;
}

void thread_yield(void)
{
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void thread_sleep_ms(uint64_t ms)
{
    uint64_t flags = irq_save();
    spin_lock(&runq_lock);
    sched_block_switch(sched_deadline(ms));
    irq_restore(flags);
}

void thread_block(void)
{
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
        ready_enqueue(t);
        percpu_get()->need_resched = true;
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

void sched_tick(uint64_t now)
{
    if (!enabled)
        return;

    spin_lock(&runq_lock);
    struct percpu *cpu = percpu_get();
    struct thread *cur = cpu->current;
    cpu->ticks++;
    cur->total_ticks++;

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
                ready_enqueue(t);
            } else {
                pp = &t->next;
            }
        }
    }

    if (cur->is_idle) {
        if (ready_head)
            cpu->need_resched = true;
    } else if (cur->slice_left == 0 || --cur->slice_left == 0) {
        cpu->need_resched = true;
    } else if (ready_head && !cur->is_idle) {
        for (uint32_t i = 0; i < MAX_CPUS; i++) {
            struct percpu *c = percpu_by_index(i);
            if (c && c->online && c->current && c->current->is_idle) {
                c->need_resched = true;
                break;
            }
        }
    }
    spin_unlock(&runq_lock);
}

void sched_check_preempt(void)
{
    if (enabled && percpu_ready() && percpu_get()->need_resched)
        schedule();
}

void sched_dump(void)
{
    uint64_t flags = spin_lock_irqsave(&runq_lock);
    kprintf("  tid  estado    cpu  ticks     nome\n");
    for (struct thread *t = all_threads; t; t = t->all_next)
        kprintf("  %3u  %-8s  %3u  %8lu  %s%s\n", t->tid, state_name[t->state], t->cpu,
                t->total_ticks, t->name, t->process ? " [user]" : "");
    spin_unlock_irqrestore(&runq_lock, flags);
}
