#include <kernel/futex.h>
#include <kernel/mutex.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <arch/x86_64/cpu.h>
#include <mm/memory.h>
#include <mm/uaccess.h>
#include <mm/vma.h>
#include <mm/vmm.h>

#define FUTEX_BUCKETS      64
#define FUTEX_MAX_SECONDS  1000000000ull
#define FUTEX_MAX_SLEEP_MS 86400000ull
#define FUTEX_READ_RETRIES 64

struct futex_key {
    uint64_t space;
    uint64_t addr;
};

struct futex_waiter {
    struct futex_key key;
    struct waitqueue wq;
    bool woken;
    struct futex_waiter *next;
};

struct futex_timeout {
    bool has;
    bool zero;
    uint64_t ms;
};

static spinlock_t futex_lock = SPINLOCK_INIT;
static struct futex_waiter *buckets[FUTEX_BUCKETS];
static uint64_t nwaiters;

static struct futex_waiter **bucket_of(const struct futex_key *k)
{
    uint64_t h = (k->addr >> 2) ^ (k->space >> 4) ^ (k->addr >> 12);
    return &buckets[h % FUTEX_BUCKETS];
}

static bool key_eq(const struct futex_key *a, const struct futex_key *b)
{
    return a->space == b->space && a->addr == b->addr;
}

static long check_addr(const uint32_t *uaddr)
{
    if ((uint64_t)uaddr & 3)
        return -EINVAL;
    if (!user_range_ok((uint64_t)uaddr, sizeof(uint32_t)))
        return -EFAULT;
    return 0;
}

static void make_key(uint32_t *uaddr, bool private_key, struct futex_key *k)
{
    struct process *p = process_current();
    k->space = (uint64_t)p;
    k->addr = (uint64_t)uaddr;
    if (private_key || !interrupts_enabled())
        return;
    mutex_lock(&p->mm_lock);
    struct vma *v = vma_find(p, (uint64_t)uaddr);
    if (v && (v->flags & VMA_SHARED)) {
        uint64_t pte = vmm_get_pte_in(p->pml4_phys, PAGE_ALIGN_DOWN((uint64_t)uaddr));
        if (pte & PTE_PRESENT) {
            k->space = 0;
            k->addr = (pte & PTE_ADDR_MASK) + ((uint64_t)uaddr & (PAGE_SIZE - 1));
        }
    }
    mutex_unlock(&p->mm_lock);
}

static void enqueue(struct futex_waiter *w)
{
    struct futex_waiter **pp = bucket_of(&w->key);
    while (*pp)
        pp = &(*pp)->next;
    w->next = NULL;
    *pp = w;
    nwaiters++;
}

static void dequeue(struct futex_waiter *w)
{
    struct futex_waiter **pp = bucket_of(&w->key);
    while (*pp && *pp != w)
        pp = &(*pp)->next;
    if (*pp) {
        *pp = w->next;
        nwaiters--;
    }
}

static int wake_locked(const struct futex_key *k, int count)
{
    int woken = 0;
    struct futex_waiter **pp = bucket_of(k);
    while (*pp && woken < count) {
        struct futex_waiter *w = *pp;
        if (!key_eq(&w->key, k)) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        nwaiters--;
        w->woken = true;
        wake_up_all(&w->wq);
        woken++;
    }
    return woken;
}

static bool lock_and_read(uint32_t *uaddr, uint32_t *out, uint64_t *flags)
{
    for (int i = 0; i < FUTEX_READ_RETRIES; i++) {
        if (copy_from_user(out, uaddr, sizeof(*out)) < 0)
            return false;
        *flags = spin_lock_irqsave(&futex_lock);
        if (copy_from_user(out, uaddr, sizeof(*out)) == 0)
            return true;
        spin_unlock_irqrestore(&futex_lock, *flags);
    }
    return false;
}

static int read_timeout(const struct user_timespec *uto, struct futex_timeout *t)
{
    t->has = false;
    t->zero = false;
    t->ms = 0;
    if (!uto)
        return 0;
    struct user_timespec ts;
    if (copy_from_user(&ts, uto, sizeof(ts)) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    if ((uint64_t)ts.tv_sec >= FUTEX_MAX_SECONDS)
        return 0;
    t->has = true;
    t->zero = ts.tv_sec == 0 && ts.tv_nsec == 0;
    t->ms = (uint64_t)ts.tv_sec * 1000 + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
    return 0;
}

static long futex_wait(uint32_t *uaddr, uint32_t val, const struct user_timespec *uto, bool private_key)
{
    struct futex_timeout to;
    int r = read_timeout(uto, &to);
    if (r < 0)
        return r;

    struct futex_waiter w;
    make_key(uaddr, private_key, &w.key);
    waitqueue_init(&w.wq);
    w.woken = false;
    w.next = NULL;

    uint32_t cur;
    uint64_t flags;
    if (!lock_and_read(uaddr, &cur, &flags))
        return -EFAULT;
    if (cur != val) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -EAGAIN;
    }
    if (to.zero) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -ETIMEDOUT;
    }
    uint64_t deadline = to.has ? time_uptime_ms() + to.ms + 1 : 0;
    enqueue(&w);
    long ret = 0;
    for (;;) {
        uint64_t left = 0;
        if (to.has) {
            uint64_t now = time_uptime_ms();
            if (now >= deadline) {
                ret = -ETIMEDOUT;
                break;
            }
            left = deadline - now;
            if (left > FUTEX_MAX_SLEEP_MS)
                left = FUTEX_MAX_SLEEP_MS;
        }
        wait_sleep_intr(&w.wq, &futex_lock, left);
        if (w.woken)
            break;
        if (signal_pending_current()) {
            ret = -EINTR;
            break;
        }
    }
    if (w.woken)
        ret = 0;
    else
        dequeue(&w);
    spin_unlock_irqrestore(&futex_lock, flags);
    return ret;
}

long futex_wake(uint32_t *uaddr, int count, bool private_key)
{
    if (count <= 0)
        count = 1;
    struct futex_key k;
    make_key(uaddr, private_key, &k);
    uint64_t flags = spin_lock_irqsave(&futex_lock);
    int n = wake_locked(&k, count);
    spin_unlock_irqrestore(&futex_lock, flags);
    return n;
}

static long futex_requeue(uint32_t *uaddr, int nwake, int nmove, uint32_t *uaddr2, bool cmp, uint32_t val3,
                          bool private_key)
{
    if (nwake < 0 || nmove < 0)
        return -EINVAL;
    long r = check_addr(uaddr2);
    if (r < 0)
        return r;
    struct futex_key k1, k2;
    make_key(uaddr, private_key, &k1);
    make_key(uaddr2, private_key, &k2);

    uint64_t flags;
    if (cmp) {
        uint32_t cur;
        if (!lock_and_read(uaddr, &cur, &flags))
            return -EFAULT;
        if (cur != val3) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -EAGAIN;
        }
    } else {
        flags = spin_lock_irqsave(&futex_lock);
    }
    int woken = wake_locked(&k1, nwake);
    int moved = 0;
    struct futex_waiter **pp = bucket_of(&k1);
    bool same = key_eq(&k1, &k2);
    while (*pp && moved < nmove) {
        struct futex_waiter *w = *pp;
        if (!key_eq(&w->key, &k1)) {
            pp = &w->next;
            continue;
        }
        moved++;
        if (same) {
            pp = &w->next;
            continue;
        }
        *pp = w->next;
        nwaiters--;
        w->key = k2;
        enqueue(w);
    }
    spin_unlock_irqrestore(&futex_lock, flags);
    return woken + moved;
}

long futex_syscall(uint32_t *uaddr, int op, uint32_t val, uint64_t arg4, uint32_t *uaddr2, uint32_t val3)
{
    long r = check_addr(uaddr);
    if (r < 0)
        return r;
    bool private_key = (op & FUTEX_PRIVATE_FLAG) != 0;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT)
        return -ENOSYS;
    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, (const struct user_timespec *)arg4, private_key);
    case FUTEX_WAKE:
        return futex_wake(uaddr, (int)val, private_key);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, (int)val, (int)arg4, uaddr2, false, 0, private_key);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, (int)val, (int)arg4, uaddr2, true, val3, private_key);
    default:
        return -ENOSYS;
    }
}

uint64_t futex_waiters(void)
{
    return __atomic_load_n(&nwaiters, __ATOMIC_RELAXED);
}
