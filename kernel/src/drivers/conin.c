#include <drivers/conin.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

static spinlock_t lock = SPINLOCK_INIT;
static struct waitqueue wq = WAITQUEUE_INIT;
static volatile uint32_t pending;
static uint64_t wakeups;
static uint64_t timeouts;

void conin_kick(void)
{
    __atomic_store_n(&pending, 1, __ATOMIC_RELEASE);
    wake_up(&wq);
}

void conin_wait(uint64_t ms)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    if (!__atomic_exchange_n(&pending, 0, __ATOMIC_ACQ_REL)) {
        if (wait_sleep_timeout(&wq, &lock, ms))
            __atomic_add_fetch(&wakeups, 1, __ATOMIC_RELAXED);
        else
            __atomic_add_fetch(&timeouts, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&pending, 0, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&lock, flags);
}

uint64_t conin_wakeups(void)
{
    return __atomic_load_n(&wakeups, __ATOMIC_RELAXED);
}

uint64_t conin_timeouts(void)
{
    return __atomic_load_n(&timeouts, __ATOMIC_RELAXED);
}
