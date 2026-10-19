#pragma once

#include <kernel/compiler.h>
#include <arch/x86_64/cpu.h>

typedef struct {
    volatile uint32_t locked;
    volatile uint32_t owner_cpu;
    volatile uint64_t owner_pc;
    void *volatile owner_thread;
} spinlock_t;

#define SPINLOCK_INIT { 0, 0, 0, NULL }

#define SPIN_HERE() ({ __label__ here_; here_: (uint64_t)&&here_; })

void spin_lock_slow(spinlock_t *l);
void spin_set_owner(spinlock_t *l, uint64_t pc);
void spin_owner_release(void *thread);
bool spin_held(spinlock_t *l);
void spin_set_timeout(uint64_t tsc_hz);

static inline void spinlock_init(spinlock_t *l)
{
    l->locked = 0;
    l->owner_cpu = 0;
    l->owner_pc = 0;
    l->owner_thread = NULL;
}

static inline __attribute__((always_inline)) bool spin_try_lock(spinlock_t *l)
{
    if (__sync_lock_test_and_set(&l->locked, 1) != 0)
        return false;
    spin_set_owner(l, SPIN_HERE());
    return true;
}

static inline __attribute__((always_inline)) void spin_lock(spinlock_t *l)
{
    if (unlikely(__sync_lock_test_and_set(&l->locked, 1) != 0))
        spin_lock_slow(l);
    spin_set_owner(l, SPIN_HERE());
}

static inline void spin_unlock(spinlock_t *l)
{
    void *owner = l->owner_thread;
    l->owner_cpu = 0;
    l->owner_thread = NULL;
    if (owner)
        spin_owner_release(owner);
    __sync_lock_release(&l->locked);
}

#define ASSERT_HELD(l) ASSERT(spin_held(l))

static inline __attribute__((always_inline)) uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags = irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}