#include <kernel/file.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <fs/devfs.h>
#include <drivers/tty.h>
#include <mm/heap.h>
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

int64_t file_mmap(struct file *f, uint64_t len, int prot, uint64_t offset)
{
    return f->ops->mmap ? f->ops->mmap(f, len, prot, offset) : -ENODEV;
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

static const struct file_ops tty_ops = {
    .read = tty_file_read,
    .write = tty_file_write,
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

static const struct file_ops vfs_ops = {
    .read = vfs_file_read,
    .readdir = vfs_file_readdir,
};

struct file *file_open_path(const char *path)
{
    if (devfs_owns(path))
        return devfs_open(path);
    struct vfs_node node;
    if (!vfs_lookup(path, &node))
        return NULL;
    struct file *f = file_alloc(&vfs_ops, NULL);
    if (!f)
        return NULL;
    f->node = node;
    f->has_node = true;
    return f;
}

#define PIPE_SIZE 4096

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint64_t head, tail;
    int32_t readers, writers;
    spinlock_t lock;
};

static int64_t pipe_read(struct file *f, void *buf, uint64_t len)
{
    struct pipe *p = f->priv;
    uint8_t *out = buf;
    uint64_t n = 0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        while (n < len && p->tail != p->head) {
            out[n++] = p->buf[p->tail % PIPE_SIZE];
            p->tail++;
        }
        bool empty = p->tail == p->head;
        int32_t writers = p->writers;
        spin_unlock_irqrestore(&p->lock, flags);
        if (n > 0 || (empty && writers == 0))
            return (int64_t)n;
        thread_sleep_ms(2);
    }
}

static int64_t pipe_write(struct file *f, const void *buf, uint64_t len)
{
    struct pipe *p = f->priv;
    const uint8_t *in = buf;
    uint64_t n = 0;
    while (n < len) {
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return n ? (int64_t)n : -EPIPE;
        }
        while (n < len && p->head - p->tail < PIPE_SIZE) {
            p->buf[p->head % PIPE_SIZE] = in[n++];
            p->head++;
        }
        spin_unlock_irqrestore(&p->lock, flags);
        if (n < len)
            thread_sleep_ms(2);
    }
    return (int64_t)n;
}

static void pipe_close_read(struct file *f)
{
    struct pipe *p = f->priv;
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->readers--;
    bool dead = p->readers == 0 && p->writers == 0;
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
    spin_unlock_irqrestore(&p->lock, flags);
    if (dead)
        kfree(p);
}

static const struct file_ops pipe_read_ops = { .read = pipe_read, .close = pipe_close_read };
static const struct file_ops pipe_write_ops = { .write = pipe_write, .close = pipe_close_write };

bool file_make_pipe(struct file **reader, struct file **writer)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return false;
    spinlock_init(&p->lock);
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
