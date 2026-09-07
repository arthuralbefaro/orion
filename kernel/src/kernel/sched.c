#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <mm/heap.h>
#include <lib/string.h>
#include <arch/x86_64/cpu.h>

extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

static struct thread *current;
static struct thread *idle_thread;
static struct thread *all_threads;
static struct thread *ready_head, *ready_tail;
static struct thread *sleeping;
static struct thread *zombie;
static uint32_t next_tid = 1;
static volatile bool need_resched;
static bool enabled;
static uint64_t context_switches;

static const char *const state_name[] = {
    [THREAD_READY]    = "ready",
    [THREAD_RUNNING]  = "running",
    [THREAD_SLEEPING] = "sleeping",
    [THREAD_BLOCKED]  = "blocked",
    [THREAD_ZOMBIE]   = "zombie",
};

static void ready_enqueue(struct thread *t)
{
    t->next = NULL;
    if (ready_tail)
        ready_tail->next = t;
    else
        ready_head = t;
    ready_tail = t;
}

static struct thread *ready_dequeue(void)
{
    struct thread *t = ready_head;
    if (t) {
        ready_head = t->next;
        if (!ready_head)
            ready_tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void reap_zombie(void)
{
    struct thread *z = zombie;
    if (!z)
        return;
    zombie = NULL;

    struct thread **pp = &all_threads;
    while (*pp && *pp != z)
        pp = &(*pp)->all_next;
    if (*pp)
        *pp = z->all_next;

    kfree(z->stack);
    kfree(z);
}

static struct thread *thread_alloc(const char *name)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->tid = next_tid++;
    strlcpy(t->name, name, sizeof(t->name));
    t->slice_left = SCHED_TIME_SLICE;

    uint64_t flags = irq_save();
    t->all_next = all_threads;
    all_threads = t;
    irq_restore(flags);
    return t;
}

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg)
{
    struct thread *t = thread_alloc(name);
    if (!t)
        return NULL;

    t->stack_size = THREAD_STACK_SIZE;
    t->stack = kmalloc(t->stack_size);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }

    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack + t->stack_size);
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0x202;
    *--sp = 0;                       
    *--sp = 0;                   
    *--sp = (uint64_t)entry;       
    *--sp = (uint64_t)arg;          
    *--sp = 0;                     
    *--sp = 0;                   
    t->rsp = (uint64_t)sp;

    uint64_t flags = irq_save();
    t->state = THREAD_READY;
    ready_enqueue(t);
    irq_restore(flags);
    return t;
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
    struct thread *main_thread = thread_alloc("main");
    main_thread->state = THREAD_RUNNING;
    current = main_thread;

    idle_thread = thread_create("idle", idle_loop, NULL);
    ready_dequeue();
    idle_thread->state = THREAD_READY;

    enabled = true;
    log_info("sched", "escalonador ativo: fatia de %u ms, thread main tid=%u, idle tid=%u",
             SCHED_TIME_SLICE * 1000 / TIME_TICK_HZ, main_thread->tid, idle_thread->tid);
}

bool sched_enabled(void)
{
    return enabled;
}

struct thread *thread_current(void)
{
    return current;
}

void schedule(void)
{
    ASSERT(!interrupts_enabled());
    reap_zombie();

    struct thread *prev = current;
    struct thread *next = ready_dequeue();

    if (!next) {
        if (prev->state == THREAD_RUNNING)
            return;
        next = idle_thread;
    }

    if (prev->state == THREAD_RUNNING && prev != idle_thread) {
        prev->state = THREAD_READY;
        ready_enqueue(prev);
    } else if (prev == idle_thread) {
        prev->state = THREAD_READY;
    } else if (prev->state == THREAD_ZOMBIE) {
        zombie = prev;
    }

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }

    next->state = THREAD_RUNNING;
    next->slice_left = SCHED_TIME_SLICE;
    need_resched = false;
    current = next;
    context_switches++;

    context_switch(&prev->rsp, next->rsp);

    reap_zombie();
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
    current->wake_at = time_ticks() + DIV_ROUND_UP(ms * TIME_TICK_HZ, 1000);
    current->state = THREAD_SLEEPING;
    current->next = sleeping;
    sleeping = current;
    schedule();
    irq_restore(flags);
}

void thread_block(void)
{
    uint64_t flags = irq_save();
    current->state = THREAD_BLOCKED;
    schedule();
    irq_restore(flags);
}

void thread_unblock(struct thread *t)
{
    uint64_t flags = irq_save();
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        ready_enqueue(t);
        need_resched = true;
    }
    irq_restore(flags);
}

NORETURN void thread_exit(void)
{
    cli();
    if (current == idle_thread || !current->stack)
        panic("thread_exit chamado pela thread %s", current->name);
    current->state = THREAD_ZOMBIE;
    schedule();
    panic("thread zombie voltou a executar");
}

void sched_tick(uint64_t now)
{
    if (!enabled)
        return;

    current->total_ticks++;

    struct thread **pp = &sleeping;
    while (*pp) {
        struct thread *t = *pp;
        if (t->wake_at <= now) {
            *pp = t->next;
            t->state = THREAD_READY;
            ready_enqueue(t);
            need_resched = true;
        } else {
            pp = &t->next;
        }
    }

    if (current == idle_thread) {
        if (ready_head)
            need_resched = true;
    } else if (current->slice_left == 0 || --current->slice_left == 0) {
        need_resched = true;
    }
}

void sched_check_preempt(void)
{
    if (enabled && need_resched)
        schedule();
}

void sched_dump(void)
{
    uint64_t flags = irq_save();
    kprintf("  tid  estado    ticks     nome\n");
    for (struct thread *t = all_threads; t; t = t->all_next)
        kprintf("  %3u  %-8s  %8lu  %s%s\n", t->tid, state_name[t->state], t->total_ticks,
                t->name, t == current ? " (atual)" : "");
    kprintf("  %lu trocas de contexto\n", context_switches);
    irq_restore(flags);
}