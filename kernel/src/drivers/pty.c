#include <drivers/pty.h>
#include <drivers/ldisc.h>
#include <kernel/file.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <lib/printf.h>
#include <mm/heap.h>
#include <lib/string.h>

#define PTY_OUT_MAX 8192

struct pty {
    struct line_discipline ld;
    spinlock_t lock;
    struct waitqueue readers;
    struct waitqueue writers;
    uint8_t out[PTY_OUT_MAX];
    uint32_t head;
    uint32_t tail;
    int32_t masters;
    int32_t slaves;
    struct user_winsize size;
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
    wake_up_all(&p->readers);
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
    for (;;) {
        uint64_t n = 0;
        while (n < len && p->tail != p->head) {
            out[n++] = p->out[p->tail];
            p->tail = (p->tail + 1) % PTY_OUT_MAX;
        }
        if (n) {
            spin_unlock_irqrestore(&p->lock, flags);
            wake_up_all(&p->writers);
            return (int64_t)n;
        }
        if (p->slaves == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -EAGAIN;
        }
        wait_sleep(&p->readers, &p->lock);
        if (signal_pending_current()) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -EINTR;
        }
    }
}

static int64_t master_write(struct file *f, const void *buf, uint64_t len)
{
    struct pty *p = f->priv;
    ldisc_write(&p->ld, buf, len);
    return (int64_t)len;
}

static short master_poll(struct file *f, struct poll_table *pt)
{
    struct pty *p = f->priv;
    poll_wait(pt, &p->readers);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    short revents = 0;
    if (p->tail != p->head)
        revents |= POLLIN;
    if (p->slaves == 0)
        revents |= POLLIN | POLLHUP;
    revents |= POLLOUT;
    spin_unlock_irqrestore(&p->lock, flags);
    return revents;
}

static long pty_pgrp_ioctl(struct pty *p, uint64_t request, uint64_t arg)
{
    if (arg >= USER_ADDR_MAX)
        return -EFAULT;
    if (request == TIOCGPGRP) {
        *(int32_t *)arg = ldisc_get_pgrp(&p->ld);
        return 0;
    }
    ldisc_set_pgrp(&p->ld, *(int32_t *)arg);
    return 0;
}

static long pty_winsize_ioctl(struct pty *p, uint64_t request, uint64_t arg)
{
    if (arg >= USER_ADDR_MAX)
        return -EFAULT;
    struct user_winsize *ws = (struct user_winsize *)arg;
    if (request == TIOCGWINSZ)
        *ws = p->size;
    else
        p->size = *ws;
    return 0;
}

long ldisc_termios_ioctl(struct line_discipline *ld, uint64_t request, uint64_t arg)
{
    if (arg >= USER_ADDR_MAX)
        return -EFAULT;
    struct user_termios *t = (struct user_termios *)arg;
    bool canonical, echo_on;
    if (request == TCGETS) {
        ldisc_get_mode(ld, &canonical, &echo_on);
        memset(t, 0, sizeof(*t));
        t->c_lflag = (canonical ? TIO_ICANON : 0) | (echo_on ? TIO_ECHO : 0);
        return 0;
    }
    ldisc_set_mode(ld, (t->c_lflag & TIO_ICANON) != 0, (t->c_lflag & TIO_ECHO) != 0);
    return 0;
}

static long master_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    struct pty *p = f->priv;
    if (request == TIOCGPGRP || request == TIOCSPGRP)
        return pty_pgrp_ioctl(p, request, arg);
    if (request == TCGETS || request == TCSETS)
        return ldisc_termios_ioctl(&p->ld, request, arg);
    if (request == TIOCGWINSZ || request == TIOCSWINSZ)
        return pty_winsize_ioctl(p, request, arg);
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
    if (notify) {
        ldisc_hangup(&p->ld);
        wake_up_all(&p->writers);
    }
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
    uint64_t flags = spin_lock_irqsave(&p->lock);
    while (done < len && p->masters > 0) {
        uint32_t n = out_push(p, in + done, (uint32_t)(len - done));
        done += n;
        if (n)
            wake_up_all(&p->readers);
        else
            wait_sleep(&p->writers, &p->lock);
    }
    spin_unlock_irqrestore(&p->lock, flags);
    return (int64_t)len;
}

static short slave_poll(struct file *f, struct poll_table *pt)
{
    struct pty *p = f->priv;
    short revents = ldisc_poll(&p->ld, pt);
    poll_wait(pt, &p->writers);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (out_used(p) < PTY_OUT_MAX - 1 || p->masters == 0)
        revents |= POLLOUT;
    spin_unlock_irqrestore(&p->lock, flags);
    return revents;
}

static long slave_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    struct pty *p = f->priv;
    if (request == TIOCGPGRP || request == TIOCSPGRP)
        return pty_pgrp_ioctl(p, request, arg);
    if (request == TCGETS || request == TCSETS)
        return ldisc_termios_ioctl(&p->ld, request, arg);
    if (request == TIOCGWINSZ || request == TIOCSWINSZ)
        return pty_winsize_ioctl(p, request, arg);
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
    bool notify = p->slaves == 0 && !dead;
    spin_unlock_irqrestore(&p->lock, flags);
    if (notify)
        wake_up_all(&p->readers);
    if (dead)
        pty_destroy(p);
}

static const struct file_ops master_ops = {
    .read = master_read,
    .write = master_write,
    .close = master_close,
    .ioctl = master_ioctl,
    .poll = master_poll,
};

static const struct file_ops slave_ops = {
    .read = slave_read,
    .write = slave_write,
    .close = slave_close,
    .ioctl = slave_ioctl,
    .poll = slave_poll,
};

int pty_open_pair(struct file **master, struct file **slave)
{
    struct pty *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    spinlock_init(&p->lock);
    waitqueue_init(&p->readers);
    waitqueue_init(&p->writers);
    ldisc_init(&p->ld, pty_echo, p);
    p->masters = 1;
    p->slaves = 1;
    p->size.ws_row = 24;
    p->size.ws_col = 80;
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
