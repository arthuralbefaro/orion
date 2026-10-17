#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>

#define MUTEX_WARN_MS 5000
#define BOOT_OWNER ((struct thread *)1)

static spinlock_t registry_lock = SPINLOCK_INIT;
static struct mutex *registry;

static void register_locked(struct mutex *m)
{
    if (m->registered)
        return;
    spin_lock(&registry_lock);
    if (!m->registered) {
        m->next = registry;
        registry = m;
        m->registered = true;
    }
    spin_unlock(&registry_lock);
}

static struct thread *me_or_boot(void)
{
    return sched_enabled() ? thread_current() : BOOT_OWNER;
}

static const char *owner_name(const struct thread *t)
{
    if (!t)
        return "-";
    if (t == BOOT_OWNER)
        return "boot";
    return t->name;
}

void mutex_init(struct mutex *m, const char *name)
{
    spinlock_init(&m->lock);
    m->owner = NULL;
    waitqueue_init(&m->wq);
    m->name = name;
    m->acquires = 0;
    m->contended = 0;
    m->wait_us = 0;
    m->max_wait_us = 0;
    m->owner_pc = 0;
    m->registered = false;
    m->next = NULL;
    uint64_t flags = spin_lock_irqsave(&m->lock);
    register_locked(m);
    spin_unlock_irqrestore(&m->lock, flags);
}

void mutex_deinit(struct mutex *m)
{
    uint64_t flags = spin_lock_irqsave(&registry_lock);
    struct mutex **pp = &registry;
    while (*pp && *pp != m)
        pp = &(*pp)->next;
    if (*pp)
        *pp = m->next;
    m->next = NULL;
    spin_unlock_irqrestore(&registry_lock, flags);
}

void mutex_init_private(struct mutex *m, const char *name)
{
    mutex_init(m, name);
    mutex_deinit(m);
}

void mutex_lock(struct mutex *m)
{
    uint64_t pc = (uint64_t)__builtin_return_address(0);
    struct thread *me = me_or_boot();
    uint64_t flags = spin_lock_irqsave(&m->lock);
    register_locked(m);
    if (m->owner == me && me != BOOT_OWNER)
        panic("mutex: a thread %s tentou pegar de novo o mutex '%s', que ela mesma segura (pego em %p)",
              me->name, m->name, (void *)m->owner_pc);
    m->acquires++;
    if (m->owner) {
        if (me == BOOT_OWNER)
            panic("mutex: '%s' disputado antes de o escalonador existir", m->name);
        if (!(flags & RFLAGS_IF))
            panic("mutex: '%s' disputado com interrupcoes desligadas (pego em %p)", m->name,
                  (void *)pc);
        m->contended++;
        uint64_t t0 = time_now_us();
        uint64_t warned = 0;
        while (m->owner) {
            wait_sleep_timeout(&m->wq, &m->lock, MUTEX_WARN_MS);
            uint64_t waited = time_now_us() - t0;
            if (m->owner && waited > (warned + 1) * MUTEX_WARN_MS * 1000ull) {
                warned++;
                struct thread *o = m->owner;
                log_warn("mutex", "'%s': %s (tid %u) espera ha %lu s; dono %s, pego em %p", m->name,
                         me->name, me->tid, waited / 1000000, owner_name(o), (void *)m->owner_pc);
            }
        }
        uint64_t dt = time_now_us() - t0;
        m->wait_us += dt;
        if (dt > m->max_wait_us)
            m->max_wait_us = dt;
    }
    m->owner = me;
    m->owner_pc = pc;
    spin_unlock_irqrestore(&m->lock, flags);
}

bool mutex_trylock(struct mutex *m)
{
    uint64_t pc = (uint64_t)__builtin_return_address(0);
    struct thread *me = me_or_boot();
    uint64_t flags = spin_lock_irqsave(&m->lock);
    register_locked(m);
    bool ok = m->owner == NULL;
    if (ok) {
        m->acquires++;
        m->owner = me;
        m->owner_pc = pc;
    }
    spin_unlock_irqrestore(&m->lock, flags);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    struct thread *me = me_or_boot();
    uint64_t flags = spin_lock_irqsave(&m->lock);
    if (m->owner != me)
        panic("mutex: '%s' liberado por %s, mas o dono e %s", m->name, owner_name(me),
              owner_name(m->owner));
    m->owner = NULL;
    m->owner_pc = 0;
    wake_up(&m->wq);
    spin_unlock_irqrestore(&m->lock, flags);
}

bool mutex_held(const struct mutex *m)
{
    return m->owner && m->owner == me_or_boot();
}

void mutex_dump(void)
{
    uint64_t flags = spin_lock_irqsave(&registry_lock);
    kprintf("  %-16s %10s %10s %10s %9s  %s\n", "mutex", "pegou", "contencao", "espera ms",
            "max ms", "dono");
    for (struct mutex *m = registry; m; m = m->next)
        kprintf("  %-16s %10lu %10lu %10lu %9lu  %s\n", m->name, m->acquires, m->contended,
                m->wait_us / 1000, m->max_wait_us / 1000, owner_name(m->owner));
    spin_unlock_irqrestore(&registry_lock, flags);
}
