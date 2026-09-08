#include <drivers/pty.h>
#include <drivers/ldisc.h>
#include <kernel/file.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <lib/printf.h>
#include <mm/heap.h>

#define PTY_OUT_MAX 8192
#define POLL_MS     2

struct pty {
    struct line_discipline ld;
    spinlock_t lock;
    uint8_t out[PTY_OUT_MAX];
    uint32_t head;
    uint32_t tail;
    int32_t masters;
    int32_t slaves;
};

static uint32_t next_id = 1;
static uint32_t live;

static uint32_t out_used(const struct pty *p)
{
    return (p->head - p->tail + PTY_OUT_MAX) % PTY_OUT_MAX;
}

static uint32_t out_push(struct pty *p, const uint8_t *src, uint32_t len)
{
    uint32_t n = 0;
    while (n < len) {
        uint32_t next = (p->head + 1) % PTY_OUT_MAX;
        if (next == p->tail)
            break;
        p->out[p->head] = src[n++];
        p->head = next;
    }
    return n;
}

static void pty_echo(void *ctx, const char *data, uint64_t len)
{
    struct pty *p = ctx;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    out_push(p, (const uint8_t *)data, (uint32_t)len);
    spin_unlock_irqrestore(&p->lock, flags);
}

static void pty_destroy(struct pty *p)
{
    __atomic_fetch_sub(&live, 1, __ATOMIC_SEQ_CST);
    kfree(p);
}

static int64_t master_read(struct file *f, void *buf, uint64_t len)
{
    struct pty *p = f->priv;
    uint8_t *out = buf;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    uint64_t n = 0;
    while (n < len && p->tail != p->head) {
        out[n++] = p->out[p->tail];
        p->tail = (p->tail + 1) % PTY_OUT_MAX;
    }
    bool orphan = p->slaves == 0;
    spin_unlock_irqrestore(&p->lock, flags);

    if (n)
        return (int64_t)n;
    return orphan ? 0 : -EAGAIN;
}

static int64_t master_write(struct file *f, const void *buf, uint64_t len)
{
    struct pty *p = f->priv;
    ldisc_write(&p->ld, buf, len);
    return (int64_t)len;
}

static long master_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    struct pty *p = f->priv;
    if (request == TIOCINQ) {
        if (arg >= USER_ADDR_MAX)
            return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&p->lock);
        uint32_t n = out_used(p);
        spin_unlock_irqrestore(&p->lock, flags);
        *(uint32_t *)arg = n;
        return 0;
    }
    return 0;
}

static void master_close(struct file *f)
{
    struct pty *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->masters--;
    bool dead = p->masters == 0 && p->slaves == 0;
    bool notify = p->masters == 0 && !dead;
    spin_unlock_irqrestore(&p->lock, flags);
    if (notify)
        ldisc_hangup(&p->ld);
    if (dead)
        pty_destroy(p);
}

static int64_t slave_read(struct file *f, void *buf, uint64_t len)
{
    struct pty *p = f->priv;
    return ldisc_read(&p->ld, buf, len);
}

static int64_t slave_write(struct file *f, const void *buf, uint64_t len)
{
    struct pty *p = f->priv;
    const uint8_t *in = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (p->masters == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return (int64_t)len;
        }
        done += out_push(p, in + done, (uint32_t)(len - done));
        spin_unlock_irqrestore(&p->lock, flags);
        if (done < len)
            thread_sleep_ms(POLL_MS);
    }
    return (int64_t)len;
}

static long slave_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    struct pty *p = f->priv;
    if (request != TIOCINQ)
        return 0;
    if (arg >= USER_ADDR_MAX)
        return -EFAULT;
    *(uint32_t *)arg = ldisc_available(&p->ld);
    return 0;
}

static void slave_close(struct file *f)
{
    struct pty *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->slaves--;
    bool dead = p->masters == 0 && p->slaves == 0;
    spin_unlock_irqrestore(&p->lock, flags);
    if (dead)
        pty_destroy(p);
}

static const struct file_ops master_ops = {
    .read = master_read,
    .write = master_write,
    .close = master_close,
    .ioctl = master_ioctl,
};

static const struct file_ops slave_ops = {
    .read = slave_read,
    .write = slave_write,
    .close = slave_close,
    .ioctl = slave_ioctl,
};

int pty_open_pair(struct file **master, struct file **slave)
{
    struct pty *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    spinlock_init(&p->lock);
    ldisc_init(&p->ld, pty_echo, p);
    p->masters = 1;
    p->slaves = 1;
    __atomic_fetch_add(&next_id, 1, __ATOMIC_SEQ_CST);

    *master = file_alloc(&master_ops, p);
    *slave = file_alloc(&slave_ops, p);
    if (!*master || !*slave) {
        kfree(*master);
        kfree(*slave);
        kfree(p);
        return -ENOMEM;
    }
    (*master)->mode = S_IFCHR | 0600;
    (*slave)->mode = S_IFCHR | 0620;
    __atomic_fetch_add(&live, 1, __ATOMIC_SEQ_CST);
    return 0;
}

uint32_t pty_count(void)
{
    return live;
}

void pty_dump(void)
{
    kprintf("  %u pty(s) viva(s), %u criada(s) desde o boot\n", pty_count(), next_id - 1);
}
