#pragma once

#include <kernel/compiler.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

struct thread;

struct mutex {
    spinlock_t lock;
    struct thread *volatile owner;
    struct waitqueue wq;
    const char *name;
    uint64_t acquires;
    uint64_t contended;
    uint64_t wait_us;
    uint64_t max_wait_us;
    uint64_t owner_pc;
    bool registered;
    struct mutex *next;
};

#define MUTEX_INIT(n) { SPINLOCK_INIT, NULL, WAITQUEUE_INIT, (n), 0, 0, 0, 0, 0, false, NULL }

void mutex_init(struct mutex *m, const char *name);
void mutex_init_private(struct mutex *m, const char *name);
void mutex_deinit(struct mutex *m);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(const struct mutex *m);
void mutex_dump(void);
