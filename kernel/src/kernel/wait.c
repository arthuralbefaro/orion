#include <kernel/wait.h>
#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>

static void entry_attach(struct wait_entry *e, struct waitqueue *wq, struct thread *t)
{
    e->thread = t;
    e->queue = wq;
    e->woken = false;
    e->next = wq->head;
    wq->head = e;
    e->chain = t->waits;
    t->waits = e;
}

static void entry_detach(struct wait_entry *e)
{
    if (e->queue) {
        struct wait_entry **pp = &e->queue->head;
        while (*pp && *pp != e)
            pp = &(*pp)->next;
        if (*pp)
            *pp = e->next;
        e->queue = NULL;
    }
    if (e->thread) {
        struct wait_entry **pp = &e->thread->waits;
        while (*pp && *pp != e)
            pp = &(*pp)->chain;
        if (*pp)
            *pp = e->chain;
        e->thread = NULL;
    }
    e->next = NULL;
    e->chain = NULL;
}

void waitqueue_init(struct waitqueue *wq)
{
    wq->head = NULL;
}

void wait_unlink(struct thread *t)
{
    while (t->waits)
        entry_detach(t->waits);
}

bool wait_sleep_timeout(struct waitqueue *wq, spinlock_t *lock, uint64_t ms)
{
    struct wait_entry e;
    uint64_t flags = irq_save();

    sched_runq_lock();
    entry_attach(&e, wq, thread_current());
    if (lock)
        spin_unlock(lock);
    sched_block_switch(ms ? sched_deadline(ms) : 0);

    sched_runq_lock();
    bool woken = e.woken;
    entry_detach(&e);
    sched_runq_unlock();

    if (lock)
        spin_lock(lock);
    irq_restore(flags);
    return woken;
}

bool wait_sleep(struct waitqueue *wq, spinlock_t *lock)
{
    return wait_sleep_timeout(wq, lock, 0);
}

void wake_up_all_locked(struct waitqueue *wq)
{
    while (wq->head) {
        struct wait_entry *e = wq->head;
        struct thread *t = e->thread;
        e->woken = true;
        entry_detach(e);
        if (t)
            sched_make_ready(t);
    }
}

void wake_up_all(struct waitqueue *wq)
{
    uint64_t flags = irq_save();
    sched_runq_lock();
    wake_up_all_locked(wq);
    sched_runq_unlock();
    irq_restore(flags);
}

void wake_up(struct waitqueue *wq)
{
    uint64_t flags = irq_save();
    sched_runq_lock();
    if (wq->head) {
        struct wait_entry *e = wq->head;
        struct thread *t = e->thread;
        e->woken = true;
        entry_detach(e);
        if (t)
            sched_make_ready(t);
    }
    sched_runq_unlock();
    irq_restore(flags);
}

void poll_table_init(struct poll_table *pt)
{
    pt->count = 0;
    pt->registering = true;
}

void poll_wait(struct poll_table *pt, struct waitqueue *wq)
{
    if (!pt || !pt->registering || pt->count >= POLL_TABLE_MAX)
        return;
    struct wait_entry *e = &pt->entries[pt->count++];
    uint64_t flags = irq_save();
    sched_runq_lock();
    entry_attach(e, wq, thread_current());
    sched_runq_unlock();
    irq_restore(flags);
}

void poll_table_clear(struct poll_table *pt)
{
    uint64_t flags = irq_save();
    sched_runq_lock();
    for (int i = 0; i < pt->count; i++)
        entry_detach(&pt->entries[i]);
    sched_runq_unlock();
    irq_restore(flags);
    pt->count = 0;
    pt->registering = true;
}

bool poll_table_sleep(struct poll_table *pt, uint64_t ms)
{
    uint64_t flags = irq_save();
    sched_runq_lock();

    bool woken = false;
    for (int i = 0; i < pt->count; i++)
        if (pt->entries[i].woken)
            woken = true;

    if (!woken) {
        sched_block_switch(ms ? sched_deadline(ms) : 0);
        sched_runq_lock();
        for (int i = 0; i < pt->count; i++)
            if (pt->entries[i].woken)
                woken = true;
    }

    for (int i = 0; i < pt->count; i++)
        entry_detach(&pt->entries[i]);
    sched_runq_unlock();
    irq_restore(flags);

    pt->count = 0;
    pt->registering = true;
    return woken;
}
