#include <net/unix.h>
#include <net/socket.h>
#include <fs/vfs.h>
#include <kernel/file.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/uaccess.h>

#define UNIX_BUF     32768
#define UNIX_BACKLOG 64
#define UNIX_WAIT_MS 1000

enum { US_NEW, US_LISTEN, US_CONNECTED, US_CLOSED };

struct unix_sock {
    int state;
    struct unix_sock *peer;
    bool peer_gone;
    bool shut_rd;
    bool shut_wr;
    bool peer_shut_wr;
    uint8_t *buf;
    uint32_t head;
    uint32_t count;
    struct unix_sock *queue[UNIX_BACKLOG];
    int qhead;
    int qcount;
    int backlog;
    struct vnode *vn;
    char path[UNIX_PATH_MAX];
    char peer_path[UNIX_PATH_MAX];
    uint32_t ev_seq;
    struct waitqueue wq;
    struct unix_sock *next;
};

static spinlock_t unix_lock = SPINLOCK_INIT;
static struct unix_sock *listeners;

static void us_wake(struct unix_sock *u)
{
    if (!u)
        return;
    u->ev_seq++;
    wake_up_all(&u->wq);
}

static struct unix_sock *us_new(void)
{
    struct unix_sock *u = kzalloc(sizeof(*u));
    if (!u)
        return NULL;
    u->buf = kmalloc(UNIX_BUF);
    if (!u->buf) {
        kfree(u);
        return NULL;
    }
    waitqueue_init(&u->wq);
    return u;
}

static void us_free(struct unix_sock *u)
{
    kfree(u->buf);
    kfree(u);
}

static void us_disconnect_locked(struct unix_sock *u)
{
    if (u->peer) {
        u->peer->peer = NULL;
        u->peer->peer_gone = true;
        us_wake(u->peer);
        u->peer = NULL;
    }
}

static long ev_wait(struct unix_sock *u, uint32_t seen)
{
    uint64_t flags = spin_lock_irqsave(&unix_lock);
    if (u->ev_seq == seen)
        wait_sleep_intr(&u->wq, &unix_lock, UNIX_WAIT_MS);
    spin_unlock_irqrestore(&unix_lock, flags);
    return signal_pending_current() ? -ERESTARTSYS : 0;
}

static int64_t unix_read(struct file *f, void *buf, uint64_t len)
{
    struct unix_sock *u = f->priv;
    if (len == 0)
        return 0;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&unix_lock);
        uint32_t seen = u->ev_seq;
        if (u->state != US_CONNECTED && !u->count) {
            spin_unlock_irqrestore(&unix_lock, flags);
            return u->state == US_CLOSED ? 0 : -ENOTCONN;
        }
        if (u->count) {
            uint64_t n = len < u->count ? len : u->count;
            for (uint64_t i = 0; i < n; i++)
                ((uint8_t *)buf)[i] = u->buf[(u->head + i) % UNIX_BUF];
            u->head = (uint32_t)((u->head + n) % UNIX_BUF);
            u->count -= (uint32_t)n;
            us_wake(u->peer);
            spin_unlock_irqrestore(&unix_lock, flags);
            return (int64_t)n;
        }
        bool eof = u->shut_rd || u->peer_gone || u->peer_shut_wr;
        spin_unlock_irqrestore(&unix_lock, flags);
        if (eof)
            return 0;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        long w = ev_wait(u, seen);
        if (w < 0)
            return w;
    }
}

static int64_t unix_write(struct file *f, const void *buf, uint64_t len)
{
    struct unix_sock *u = f->priv;
    uint64_t done = 0;
    while (done < len) {
        uint64_t flags = spin_lock_irqsave(&unix_lock);
        uint32_t seen = u->ev_seq;
        struct unix_sock *p = u->peer;
        if (u->state != US_CONNECTED || u->shut_wr || !p || p->shut_rd) {
            spin_unlock_irqrestore(&unix_lock, flags);
            if (u->state != US_CONNECTED && !u->peer_gone)
                return done ? (int64_t)done : -ENOTCONN;
            return done ? (int64_t)done : -EPIPE;
        }
        uint64_t room = UNIX_BUF - p->count;
        uint64_t n = len - done < room ? len - done : room;
        for (uint64_t i = 0; i < n; i++)
            p->buf[(p->head + p->count + i) % UNIX_BUF] = ((const uint8_t *)buf)[done + i];
        p->count += (uint32_t)n;
        if (n)
            us_wake(p);
        spin_unlock_irqrestore(&unix_lock, flags);
        done += n;
        if (done == len)
            break;
        if (f->flags & O_NONBLOCK)
            return done ? (int64_t)done : -EAGAIN;
        long w = ev_wait(u, seen);
        if (w < 0)
            return done ? (int64_t)done : w;
    }
    return (int64_t)done;
}

static short unix_poll(struct file *f, struct poll_table *pt)
{
    struct unix_sock *u = f->priv;
    poll_wait(pt, &u->wq);
    uint64_t flags = spin_lock_irqsave(&unix_lock);
    short ev = 0;
    if (u->state == US_LISTEN) {
        if (u->qcount)
            ev |= POLLIN;
    } else {
        if (u->count || u->peer_gone || u->peer_shut_wr || u->shut_rd || u->state == US_CLOSED)
            ev |= POLLIN;
        if (u->state == US_CONNECTED && u->peer && !u->shut_wr && u->peer->count < UNIX_BUF)
            ev |= POLLOUT;
        if (u->peer_gone || u->state == US_CLOSED)
            ev |= POLLHUP;
    }
    spin_unlock_irqrestore(&unix_lock, flags);
    return ev;
}

static void unix_close(struct file *f)
{
    struct unix_sock *u = f->priv;
    struct unix_sock *pending[UNIX_BACKLOG];
    int np = 0;
    uint64_t flags = spin_lock_irqsave(&unix_lock);
    if (u->state == US_LISTEN) {
        struct unix_sock **pp = &listeners;
        while (*pp && *pp != u)
            pp = &(*pp)->next;
        if (*pp)
            *pp = u->next;
        while (u->qcount) {
            struct unix_sock *c = u->queue[u->qhead];
            u->qhead = (u->qhead + 1) % UNIX_BACKLOG;
            u->qcount--;
            us_disconnect_locked(c);
            pending[np++] = c;
        }
    }
    us_disconnect_locked(u);
    u->state = US_CLOSED;
    spin_unlock_irqrestore(&unix_lock, flags);
    for (int i = 0; i < np; i++)
        us_free(pending[i]);
    vnode_put(u->vn);
    us_free(u);
}

static const struct file_ops unix_ops = {
    .read = unix_read,
    .write = unix_write,
    .close = unix_close,
    .poll = unix_poll,
};

bool unix_is_file(const struct file *f)
{
    return f && f->ops == &unix_ops;
}

static long install(struct unix_sock *u, int flags)
{
    struct file *f = file_alloc(&unix_ops, u);
    if (!f) {
        us_free(u);
        return -ENOMEM;
    }
    if (flags & SOCK_NONBLOCK)
        f->flags |= O_NONBLOCK;
    int fd = process_fd_alloc(process_current(), f, 0, (flags & SOCK_CLOEXEC) != 0);
    if (fd < 0)
        file_unref(f);
    return fd;
}

long unix_socket(int type, int flags)
{
    if (type != SOCK_STREAM)
        return -EPROTONOSUPPORT;
    struct unix_sock *u = us_new();
    if (!u)
        return -ENOMEM;
    return install(u, flags);
}

long unix_socketpair(int type, int flags, int *usv)
{
    if (type != SOCK_STREAM)
        return -EPROTONOSUPPORT;
    if (!user_range_ok((uint64_t)usv, 2 * sizeof(int)))
        return -EFAULT;
    struct unix_sock *a = us_new(), *b = us_new();
    if (!a || !b) {
        if (a)
            us_free(a);
        if (b)
            us_free(b);
        return -ENOMEM;
    }
    a->state = b->state = US_CONNECTED;
    a->peer = b;
    b->peer = a;
    long fa = install(a, flags);
    if (fa < 0) {
        us_free(b);
        return fa;
    }
    long fb = install(b, flags);
    if (fb < 0) {
        process_fd_close(process_current(), (int)fa);
        return fb;
    }
    int sv[2] = { (int)fa, (int)fb };
    if (copy_to_user(usv, sv, sizeof(sv)) < 0) {
        process_fd_close(process_current(), (int)fa);
        process_fd_close(process_current(), (int)fb);
        return -EFAULT;
    }
    return 0;
}

static long read_path(const void *uaddr, uint32_t len, char *path)
{
    struct sockaddr_un a;
    if (!uaddr || len <= sizeof(uint16_t) || len > sizeof(a))
        return -EINVAL;
    memset(&a, 0, sizeof(a));
    if (copy_from_user(&a, uaddr, len) < 0)
        return -EFAULT;
    if (a.sun_family != AF_UNIX)
        return -EAFNOSUPPORT;
    size_t n = strnlen(a.sun_path, sizeof(a.sun_path));
    if (n == 0)
        return -EINVAL;
    if (n == sizeof(a.sun_path))
        return -ENAMETOOLONG;
    memcpy(path, a.sun_path, n + 1);
    return 0;
}

static struct vnode *start_for(const char *path)
{
    return path[0] == '/' ? vfs_root() : process_cwd_vnode(process_current());
}

long unix_bind(struct file *f, const void *uaddr, uint32_t len)
{
    struct unix_sock *u = f->priv;
    char path[UNIX_PATH_MAX];
    long r = read_path(uaddr, len, path);
    if (r < 0)
        return r;
    if (u->vn || u->state != US_NEW)
        return -EINVAL;
    struct vnode *start = start_for(path);
    if (!start)
        return -ENOENT;
    uint32_t mask = __atomic_load_n(&process_current()->umask, __ATOMIC_RELAXED);
    struct vnode *v = NULL;
    r = vfs_mknod(start, path, S_IFSOCK | (0777 & ~mask), &v);
    vnode_put(start);
    if (r == -EEXIST)
        return -EADDRINUSE;
    if (r < 0)
        return r;
    u->vn = v;
    strlcpy(u->path, path, sizeof(u->path));
    return 0;
}

long unix_listen(struct file *f, int backlog)
{
    struct unix_sock *u = f->priv;
    uint64_t flags = spin_lock_irqsave(&unix_lock);
    long r = 0;
    if (u->state == US_LISTEN) {
        u->backlog = backlog < 1 ? 1 : (backlog > UNIX_BACKLOG ? UNIX_BACKLOG : backlog);
    } else if (u->state != US_NEW || !u->vn) {
        r = -EINVAL;
    } else {
        u->state = US_LISTEN;
        u->backlog = backlog < 1 ? 1 : (backlog > UNIX_BACKLOG ? UNIX_BACKLOG : backlog);
        u->next = listeners;
        listeners = u;
    }
    spin_unlock_irqrestore(&unix_lock, flags);
    return r;
}

long unix_connect(struct file *f, const void *uaddr, uint32_t len)
{
    struct unix_sock *u = f->priv;
    char path[UNIX_PATH_MAX];
    long r = read_path(uaddr, len, path);
    if (r < 0)
        return r;
    if (u->state == US_CONNECTED)
        return -EISCONN;
    if (u->state != US_NEW)
        return -EINVAL;
    struct vnode *start = start_for(path);
    if (!start)
        return -ENOENT;
    struct vnode *v = NULL;
    r = vfs_walk(start, path, &v);
    vnode_put(start);
    if (r < 0)
        return r;
    struct unix_sock *s = us_new();
    if (!s) {
        vnode_put(v);
        return -ENOMEM;
    }
    uint64_t flags = spin_lock_irqsave(&unix_lock);
    struct unix_sock *l = listeners;
    while (l && l->vn != v)
        l = l->next;
    if (!l)
        r = -ECONNREFUSED;
    else if (l->qcount >= l->backlog)
        r = -EAGAIN;
    if (r == 0) {
        s->state = US_CONNECTED;
        s->peer = u;
        strlcpy(s->path, l->path, sizeof(s->path));
        u->state = US_CONNECTED;
        u->peer = s;
        strlcpy(u->peer_path, l->path, sizeof(u->peer_path));
        l->queue[(l->qhead + l->qcount) % UNIX_BACKLOG] = s;
        l->qcount++;
        us_wake(l);
    }
    spin_unlock_irqrestore(&unix_lock, flags);
    vnode_put(v);
    if (r < 0)
        us_free(s);
    return r;
}

static long put_name(const char *path, void *uaddr, uint32_t *ulen)
{
    if (!uaddr || !ulen)
        return 0;
    uint32_t room;
    if (copy_from_user(&room, ulen, sizeof(room)) < 0)
        return -EFAULT;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strlcpy(a.sun_path, path, sizeof(a.sun_path));
    uint32_t full = (uint32_t)(sizeof(a.sun_family) + strlen(path) + (path[0] ? 1 : 0));
    uint32_t n = room < full ? room : full;
    if (n && copy_to_user(uaddr, &a, n) < 0)
        return -EFAULT;
    return copy_to_user(ulen, &full, sizeof(full));
}

long unix_accept(struct file *f, void *uaddr, uint32_t *ulen, int flags)
{
    struct unix_sock *u = f->priv;
    if (u->state != US_LISTEN)
        return -EINVAL;
    struct unix_sock *c;
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&unix_lock);
        uint32_t seen = u->ev_seq;
        if (u->qcount) {
            c = u->queue[u->qhead];
            u->qhead = (u->qhead + 1) % UNIX_BACKLOG;
            u->qcount--;
            spin_unlock_irqrestore(&unix_lock, irq);
            break;
        }
        spin_unlock_irqrestore(&unix_lock, irq);
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        long w = ev_wait(u, seen);
        if (w < 0)
            return w;
    }
    long fd = install(c, flags);
    if (fd < 0) {
        uint64_t irq = spin_lock_irqsave(&unix_lock);
        if (c->peer) {
            c->peer->peer = NULL;
            c->peer->peer_gone = true;
            us_wake(c->peer);
        }
        spin_unlock_irqrestore(&unix_lock, irq);
        return fd;
    }
    if (put_name("", uaddr, ulen) < 0)
        return -EFAULT;
    return fd;
}

long unix_shutdown(struct file *f, int how)
{
    struct unix_sock *u = f->priv;
    if (u->state != US_CONNECTED)
        return -ENOTCONN;
    uint64_t irq = spin_lock_irqsave(&unix_lock);
    if (how != SHUT_WR)
        u->shut_rd = true;
    if (how != SHUT_RD) {
        u->shut_wr = true;
        if (u->peer) {
            u->peer->peer_shut_wr = true;
            us_wake(u->peer);
        }
    }
    us_wake(u);
    spin_unlock_irqrestore(&unix_lock, irq);
    return 0;
}

long unix_getsockname(struct file *f, void *uaddr, uint32_t *ulen)
{
    struct unix_sock *u = f->priv;
    return put_name(u->path, uaddr, ulen);
}

long unix_getpeername(struct file *f, void *uaddr, uint32_t *ulen)
{
    struct unix_sock *u = f->priv;
    if (u->state != US_CONNECTED)
        return -ENOTCONN;
    return put_name(u->peer_path, uaddr, ulen);
}
