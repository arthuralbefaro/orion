#include <kernel/file.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <fs/devfs.h>
#include <drivers/tty.h>
#include <drivers/pty.h>
#include <drivers/console.h>
#include <mm/heap.h>
#include <mm/uaccess.h>
#include <lib/string.h>

struct file *file_alloc(const struct file_ops *ops, void *priv)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    f->ops = ops;
    f->priv = priv;
    f->refcount = 1;
    return f;
}

void file_ref(struct file *f)
{
    __atomic_fetch_add(&f->refcount, 1, __ATOMIC_SEQ_CST);
}

void file_unref(struct file *f)
{
    if (!f)
        return;
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        if (f->ops->close)
            f->ops->close(f);
        kfree(f->path);
        kfree(f);
    }
}

int64_t file_read(struct file *f, void *buf, uint64_t len)
{
    return f->ops->read ? f->ops->read(f, buf, len) : -EBADF;
}

int64_t file_write(struct file *f, const void *buf, uint64_t len)
{
    return f->ops->write ? f->ops->write(f, buf, len) : -EBADF;
}

int file_readdir(struct file *f, uint64_t index, struct vfs_node *out)
{
    return f->ops->readdir ? f->ops->readdir(f, index, out) : -ENOTDIR;
}

long file_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    if (f->ops->ioctl)
        return f->ops->ioctl(f, request, arg);
    return file_is_tty(f) ? 0 : -ENOTTY;
}

int64_t file_mmap(struct file *f, uint64_t addr, uint64_t len, int prot, uint64_t offset)
{
    return f->ops->mmap ? f->ops->mmap(f, addr, len, prot, offset) : -ENODEV;
}

short file_poll(struct file *f, struct poll_table *pt)
{
    if (f->ops->poll)
        return f->ops->poll(f, pt);
    return POLLIN | POLLOUT;
}

static int64_t tty_file_read(struct file *f, void *buf, uint64_t len)
{
    (void)f;
    return tty_read(buf, len);
}

static int64_t tty_file_write(struct file *f, const void *buf, uint64_t len)
{
    (void)f;
    tty_write(buf, len);
    return (int64_t)len;
}

static long tty_file_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    (void)f;
    if (request == TIOCGPGRP) {
        int32_t pg = tty_get_pgrp();
        return copy_to_user((void *)arg, &pg, sizeof(pg));
    }
    if (request == TIOCSPGRP) {
        int32_t pg;
        if (copy_from_user(&pg, (const void *)arg, sizeof(pg)) < 0)
            return -EFAULT;
        tty_set_pgrp(pg);
        return 0;
    }
    if (request == TCGETS || IS_TCSETS(request))
        return tty_termios(request, arg);
    if (request == TIOCGWINSZ) {
        struct user_winsize ws;
        uint32_t cols, rows;
        console_get_size(&cols, &rows);
        ws.ws_col = (uint16_t)(cols ? cols : 80);
        ws.ws_row = (uint16_t)(rows ? rows : 24);
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        return copy_to_user((void *)arg, &ws, sizeof(ws));
    }
    return 0;
}

static short tty_file_poll(struct file *f, struct poll_table *pt)
{
    (void)f;
    return tty_poll(pt) | POLLOUT;
}

static const struct file_ops tty_ops = {
    .read = tty_file_read,
    .write = tty_file_write,
    .ioctl = tty_file_ioctl,
    .poll = tty_file_poll,
};

struct file *file_open_tty(void)
{
    struct file *f = file_alloc(&tty_ops, NULL);
    if (f)
        f->mode = S_IFCHR | 0620;
    return f;
}

bool file_is_tty(const struct file *f)
{
    return f->ops == &tty_ops;
}

static int64_t vfs_file_read(struct file *f, void *buf, uint64_t len)
{
    if (f->node.is_dir)
        return -EISDIR;
    int64_t n = vfs_read(&f->node, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n;
}

struct readdir_ctx {
    uint64_t want;
    uint64_t seen;
    struct vfs_node *out;
    bool found;
};

static bool readdir_cb(const struct vfs_node *e, void *ctx)
{
    struct readdir_ctx *r = ctx;
    if (r->seen++ == r->want) {
        *r->out = *e;
        r->found = true;
        return false;
    }
    return true;
}

static int vfs_file_readdir(struct file *f, uint64_t index, struct vfs_node *out)
{
    if (!f->node.is_dir)
        return -ENOTDIR;
    struct readdir_ctx r = { .want = index, .out = out };
    f->node.fs->ops->readdir(f->node.fs, &f->node, readdir_cb, &r);
    return r.found ? 1 : 0;
}

static int64_t vfs_file_write(struct file *f, const void *buf, uint64_t len)
{
    if (f->node.is_dir)
        return -EISDIR;
    if (f->flags & O_APPEND)
        f->offset = f->node.size;
    int64_t n = vfs_write(&f->node, f->offset, buf, len);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n < 0 ? -EIO : n;
}

static const struct file_ops vfs_ops = {
    .read = vfs_file_read,
    .write = vfs_file_write,
    .readdir = vfs_file_readdir,
};

bool file_is_regular(const struct file *f)
{
    return f->ops == &vfs_ops;
}

struct file *file_open_path(const char *path, int flags)
{
    if (devfs_owns(path))
        return devfs_open(path);

    struct vfs_node node;
    bool found = vfs_lookup(path, &node);
    if (!found) {
        if (!(flags & O_CREAT))
            return NULL;
        if (!vfs_create(path, false, &node))
            return NULL;
    } else if ((flags & O_TRUNC) && !node.is_dir && (flags & (O_WRONLY | O_RDWR))) {
        if (!vfs_truncate(&node, 0))
            return NULL;
    }

    struct file *f = file_alloc(&vfs_ops, NULL);
    if (!f)
        return NULL;
    f->node = node;
    f->has_node = true;
    f->flags = (uint32_t)flags;
    if (flags & O_APPEND)
        f->offset = node.size;
    return f;
}

#define PIPE_SIZE 4096

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint64_t head, tail;
    int32_t readers, writers;
    spinlock_t lock;
    struct waitqueue rq;
    struct waitqueue wq;
};

static int64_t pipe_read(struct file *f, void *buf, uint64_t len)
{
    struct pipe *p = f->priv;
    uint8_t *out = buf;
    uint64_t n = 0;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (;;) {
        while (n < len && p->tail != p->head) {
            out[n++] = p->buf[p->tail % PIPE_SIZE];
            p->tail++;
        }
        if (n) {
            wake_up_all(&p->wq);
            break;
        }
        if (p->writers == 0)
            break;
        if (f->flags & O_NONBLOCK) {
            n = (uint64_t)-EAGAIN;
            break;
        }
        wait_sleep(&p->rq, &p->lock);
        if (signal_pending_current() && !n) {
            n = (uint64_t)-EINTR;
            break;
        }
    }
    spin_unlock_irqrestore(&p->lock, flags);
    return (int64_t)n;
}

static int64_t pipe_write(struct file *f, const void *buf, uint64_t len)
{
    struct pipe *p = f->priv;
    const uint8_t *in = buf;
    uint64_t n = 0;
    int64_t ret;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (;;) {
        if (p->readers == 0) {
            ret = n ? (int64_t)n : -EPIPE;
            if (!n)
                signal_send(process_current(), SIGPIPE);
            break;
        }
        uint64_t before = n;
        while (n < len && p->head - p->tail < PIPE_SIZE) {
            p->buf[p->head % PIPE_SIZE] = in[n++];
            p->head++;
        }
        if (n > before)
            wake_up_all(&p->rq);
        if (n >= len) {
            ret = (int64_t)n;
            break;
        }
        if (f->flags & O_NONBLOCK) {
            ret = n ? (int64_t)n : -EAGAIN;
            break;
        }
        wait_sleep(&p->wq, &p->lock);
        if (signal_pending_current()) {
            ret = n ? (int64_t)n : -EINTR;
            break;
        }
    }
    spin_unlock_irqrestore(&p->lock, flags);
    return ret;
}

static short pipe_read_poll(struct file *f, struct poll_table *pt)
{
    struct pipe *p = f->priv;
    poll_wait(pt, &p->rq);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    short revents = 0;
    if (p->tail != p->head)
        revents |= POLLIN;
    if (p->writers == 0)
        revents |= POLLIN | POLLHUP;
    spin_unlock_irqrestore(&p->lock, flags);
    return revents;
}

static short pipe_write_poll(struct file *f, struct poll_table *pt)
{
    struct pipe *p = f->priv;
    poll_wait(pt, &p->wq);
    uint64_t flags = spin_lock_irqsave(&p->lock);
    short revents = 0;
    if (p->head - p->tail < PIPE_SIZE)
        revents |= POLLOUT;
    if (p->readers == 0)
        revents |= POLLERR;
    spin_unlock_irqrestore(&p->lock, flags);
    return revents;
}

static void pipe_close_read(struct file *f)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->readers--;
    bool dead = p->readers == 0 && p->writers == 0;
    if (!dead)
        wake_up_all(&p->wq);
    spin_unlock_irqrestore(&p->lock, flags);
    if (dead)
        kfree(p);
}

static void pipe_close_write(struct file *f)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->writers--;
    bool dead = p->readers == 0 && p->writers == 0;
    if (!dead)
        wake_up_all(&p->rq);
    spin_unlock_irqrestore(&p->lock, flags);
    if (dead)
        kfree(p);
}

static const struct file_ops pipe_read_ops = {
    .read = pipe_read, .close = pipe_close_read, .poll = pipe_read_poll
};
static const struct file_ops pipe_write_ops = {
    .write = pipe_write, .close = pipe_close_write, .poll = pipe_write_poll
};

bool file_make_pipe(struct file **reader, struct file **writer)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return false;
    spinlock_init(&p->lock);
    waitqueue_init(&p->rq);
    waitqueue_init(&p->wq);
    p->readers = 1;
    p->writers = 1;
    *reader = file_alloc(&pipe_read_ops, p);
    *writer = file_alloc(&pipe_write_ops, p);
    if (!*reader || !*writer) {
        kfree(*reader);
        kfree(*writer);
        kfree(p);
        return false;
    }
    return true;
}
