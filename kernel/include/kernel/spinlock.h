/*
 * spinlock.h - trava de exclusão mútua por espera ativa.
 *
 * Em uma CPU só, a versão *_irqsave é a que interessa: desligar interrupções
 * já impede preempção. As instruções atômicas (xchg) ficam prontas para SMP.
 */
#pragma once

#include <kernel/compiler.h>
#include <arch/x86_64/cpu.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *l)
{
    l->locked = 0;
}

static inline bool spin_try_lock(spinlock_t *l)
{
    return __sync_lock_test_and_set(&l->locked, 1) == 0;
}

static inline void spin_lock(spinlock_t *l)
{
    while (!spin_try_lock(l))
        pause();
}

static inline void spin_unlock(spinlock_t *l)
{
    __sync_lock_release(&l->locked);
}

/* Desliga interrupções, trava, e devolve o estado anterior das flags. */
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
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
