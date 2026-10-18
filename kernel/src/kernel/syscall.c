#include <drivers/power.h>
#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/file.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/protect.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <mm/uaccess.h>
#include <mm/vma.h>
#include <lib/string.h>
#include <kernel/time.h>
#include <drivers/rtc.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <drivers/pty.h>
#include <kernel/wait.h>
#include <kernel/futex.h>
#include <kernel/timesys.h>
#include <kernel/cred.h>
#include <kernel/signal.h>
#include <mm/heap.h>
#include <net/socket.h>
#include <kernel/smp.h>
#include <kernel/version.h>

extern void syscall_entry(void);

#define EFER_SCE (1ull << 0)
#define USER_PATH_MAX 512
#define EXEC_MAX_ARGS 256
#define EXEC_ARG_MAX  4096

struct upath {
    char path[USER_PATH_MAX];
    char full[PROCESS_CWD_MAX];
    bool full_ok;
    struct vnode *start;
};

static long upath_get(int dirfd, const char *upath, struct upath *up)
{
    up->start = NULL;
    up->full_ok = false;
    long r = strncpy_from_user(up->path, upath, sizeof(up->path));
    if (r < 0)
        return r;
    if (!up->path[0])
        return -ENOENT;
    struct process *p = process_current();
    if (up->path[0] == '/' || dirfd == AT_FDCWD) {
        up->full_ok = process_resolve_path(p, up->path, up->full, sizeof(up->full));
        up->start = up->path[0] == '/' ? vfs_root() : process_cwd_vnode(p);
    } else {
        struct file *dir = process_fd_pin(dirfd);
        if (!dir)
            return -EBADF;
        if (!dir->vn || !vnode_is_dir(dir->vn))
            return -ENOTDIR;
        if (dir->path) {
            char joined[USER_PATH_MAX + PROCESS_CWD_MAX + 2];
            ksnprintf(joined, sizeof(joined), "%s/%s", dir->path, up->path);
            up->full_ok = process_resolve_path(p, joined, up->full, sizeof(up->full));
        }
        vnode_get(dir->vn);
        up->start = dir->vn;
    }
    return up->start ? 0 : -ENOENT;
}

static void upath_put(struct upath *up)
{
    vnode_put(up->start);
    up->start = NULL;
}

static char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static long walk_user_ex(int dirfd, const char *upath, bool follow, struct vnode **out, struct upath *up)
{
    long r = upath_get(dirfd, upath, up);
    bool followed = false;
    if (r == 0)
        r = vfs_walk_ex(up->start, up->path, follow, out, &followed);
    if (followed)
        up->full_ok = false;
    upath_put(up);
    return r;
}

static long walk_user(int dirfd, const char *upath, struct vnode **out, struct upath *up)
{
    return walk_user_ex(dirfd, upath, true, out, up);
}

static long sys_read(int fd, void *ubuf, size_t len)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    if (len == 0)
        return 0;

    struct kbuf kb;
    uint8_t *k = kbuf_get(&kb, len);
    long total = 0;
    while ((size_t)total < len) {
        size_t want = len - (size_t)total;
        if (want > kb.size)
            want = kb.size;
        int64_t n = file_read(f, k, want);
        if (n < 0) {
            if (!total)
                total = n;
            break;
        }
        if (n > 0 && copy_to_user((uint8_t *)ubuf + total, k, (size_t)n) < 0) {
            if (!total)
                total = -EFAULT;
            break;
        }
        total += n;
        if ((size_t)n < want || !file_is_regular(f))
            break;
    }
    kbuf_put(&kb);
    return total;
}

static long positional_file(int fd, int64_t off, bool writing, struct file **out)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (off < 0)
        return -EINVAL;
    if (!f->vn || !file_is_regular(f))
        return -ESPIPE;
    if (vnode_is_dir(f->vn))
        return -EISDIR;
    uint32_t acc = f->flags & O_ACCMODE;
    if (writing ? acc == O_RDONLY : acc == O_WRONLY)
        return -EBADF;
    *out = f;
    return 0;
}

static long sys_pread64(int fd, void *ubuf, size_t len, int64_t off)
{
    struct file *f;
    long r = positional_file(fd, off, false, &f);
    if (r < 0)
        return r;
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    struct kbuf kb;
    uint8_t *k = kbuf_get(&kb, len ? len : 1);
    long total = 0;
    while ((size_t)total < len) {
        size_t want = len - (size_t)total;
        if (want > kb.size)
            want = kb.size;
        int64_t n = vfs_read(f->vn, (uint64_t)off + (uint64_t)total, k, want);
        if (n < 0) {
            if (!total)
                total = n;
            break;
        }
        if (n > 0 && copy_to_user((uint8_t *)ubuf + total, k, (size_t)n) < 0) {
            if (!total)
                total = -EFAULT;
            break;
        }
        total += n;
        if ((size_t)n < want)
            break;
    }
    kbuf_put(&kb);
    return total;
}

static long sys_pwrite64(int fd, const void *ubuf, size_t len, int64_t off)
{
    struct file *f;
    long r = positional_file(fd, off, true, &f);
    if (r < 0)
        return r;
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    struct kbuf kb;
    uint8_t *k = kbuf_get(&kb, len ? len : 1);
    long total = 0;
    while ((size_t)total < len) {
        size_t want = len - (size_t)total;
        if (want > kb.size)
            want = kb.size;
        if (copy_from_user(k, (const uint8_t *)ubuf + total, want) < 0) {
            if (!total)
                total = -EFAULT;
            break;
        }
        uint64_t at = (uint64_t)off + (uint64_t)total;
        int64_t n = vfs_write(f->vn, &at, k, want, false);
        if (n < 0) {
            if (!total)
                total = n;
            break;
        }
        total += n;
        if ((size_t)n < want)
            break;
    }
    kbuf_put(&kb);
    return total;
}

static bool append_lock(struct file *f)
{
    if (!f->vn || !file_is_regular(f) || !(f->flags & O_APPEND))
        return false;
    mutex_lock(&f->vn->io_lock);
    return true;
}

static long write_file(struct file *f, const void *ubuf, size_t len)
{
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    if (len == 0)
        return 0;

    struct kbuf kb;
    uint8_t *k = kbuf_get(&kb, len);
    long total = 0;
    while ((size_t)total < len) {
        size_t want = len - (size_t)total;
        if (want > kb.size)
            want = kb.size;
        if (copy_from_user(k, (const uint8_t *)ubuf + total, want) < 0) {
            if (!total)
                total = -EFAULT;
            break;
        }
        int64_t n = file_write(f, k, want);
        if (n < 0) {
            if (!total)
                total = n;
            break;
        }
        total += n;
        if ((size_t)n < want)
            break;
    }
    kbuf_put(&kb);
    return total;
}

static long sys_write(int fd, const void *ubuf, size_t len)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    bool locked = append_lock(f);
    long r = write_file(f, ubuf, len);
    if (locked)
        mutex_unlock(&f->vn->io_lock);
    return r;
}

static uint32_t create_mode(uint32_t mode)
{
    return mode & ~__atomic_load_n(&process_current()->umask, __ATOMIC_RELAXED) & 07777;
}

static long sys_umask(uint32_t mask)
{
    return __atomic_exchange_n(&process_current()->umask, mask & 0777, __ATOMIC_RELAXED);
}

static long sys_openat(int dirfd, const char *upath, int flags, uint32_t mode)
{
    struct upath up;
    long r = upath_get(dirfd, upath, &up);
    struct file *f = NULL;
    bool followed = false;
    if (r == 0)
        r = vfs_open_ex(up.start, up.path, flags, create_mode(mode), &f, &followed);
    upath_put(&up);
    if (r < 0)
        return r;
    f->flags = (uint32_t)flags & ~(uint32_t)(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (up.full_ok && !followed)
        f->path = kstrdup(up.full);
    int fd = process_fd_alloc(process_current(), f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static long sys_open(const char *upath, int flags, uint32_t mode)
{
    return sys_openat(AT_FDCWD, upath, flags, mode);
}

static long sys_mkdirat(int dirfd, const char *upath, uint32_t mode)
{
    struct upath up;
    long r = upath_get(dirfd, upath, &up);
    if (r == 0)
        r = vfs_mkdir(up.start, up.path, create_mode(mode));
    upath_put(&up);
    return r;
}

static long sys_mkdir(const char *upath, uint32_t mode)
{
    return sys_mkdirat(AT_FDCWD, upath, mode);
}

static long sys_unlinkat(int dirfd, const char *upath, bool is_dir)
{
    struct upath up;
    long r = upath_get(dirfd, upath, &up);
    if (r == 0)
        r = vfs_unlink(up.start, up.path, is_dir);
    upath_put(&up);
    return r;
}

static long sys_unlink(const char *upath, bool is_dir)
{
    return sys_unlinkat(AT_FDCWD, upath, is_dir);
}

static long sys_renameat(int olddir, const char *ufrom, int newdir, const char *uto)
{
    struct upath *a = kmalloc(sizeof(*a) * 2);
    if (!a)
        return -ENOMEM;
    struct upath *b = a + 1;
    b->start = NULL;
    long r = upath_get(olddir, ufrom, a);
    if (r == 0)
        r = upath_get(newdir, uto, b);
    if (r == 0)
        r = vfs_rename(a->start, a->path, b->start, b->path);
    upath_put(a);
    upath_put(b);
    kfree(a);
    return r;
}

static long sys_rename(const char *ufrom, const char *uto)
{
    return sys_renameat(AT_FDCWD, ufrom, AT_FDCWD, uto);
}

static long sys_ftruncate(int fd, uint64_t size)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if ((int64_t)size < 0)
        return -EINVAL;
    if (!f->vn || !vnode_is_reg(f->vn) || (f->flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;
    return vfs_truncate(f->vn, size);
}

static long sys_truncate(const char *upath, uint64_t size)
{
    if ((int64_t)size < 0)
        return -EINVAL;
    struct upath up;
    struct vnode *v;
    long r = walk_user(AT_FDCWD, upath, &v, &up);
    if (r < 0)
        return r;
    r = vnode_is_dir(v) ? -EISDIR : vfs_permission(v, VFS_MAY_WRITE);
    if (r == 0)
        r = vfs_truncate(v, size);
    vnode_put(v);
    return r;
}

static long sys_close(int fd)
{
    return process_fd_close(process_current(), fd);
}

static long install_pair(struct file *a, struct file *b, int *ufds, bool cloexec)
{
    struct process *p = process_current();
    int fds[2];
    fds[0] = process_fd_alloc(p, a, 0, cloexec);
    fds[1] = process_fd_alloc(p, b, 0, cloexec);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) process_fd_close_if(p, fds[0], a); else file_unref(a);
        if (fds[1] >= 0) process_fd_close_if(p, fds[1], b); else file_unref(b);
        return -EMFILE;
    }
    if (copy_to_user(ufds, fds, sizeof(fds)) < 0) {
        process_fd_close_if(p, fds[0], a);
        process_fd_close_if(p, fds[1], b);
        return -EFAULT;
    }
    return 0;
}

static long sys_pipe2(int *ufds, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;
    if (!user_range_ok((uint64_t)ufds, 2 * sizeof(int)))
        return -EFAULT;
    struct file *r, *w;
    if (!file_make_pipe(&r, &w))
        return -ENOMEM;
    if (flags & O_NONBLOCK) {
        r->flags |= O_NONBLOCK;
        w->flags |= O_NONBLOCK;
    }
    return install_pair(r, w, ufds, (flags & O_CLOEXEC) != 0);
}

static long sys_pipe(int *ufds)
{
    return sys_pipe2(ufds, 0);
}

static long sys_reboot(uint32_t magic1, uint32_t magic2, uint32_t cmd)
{
    if (!cred_is_root())
        return -EPERM;
    if (magic1 != 0xFEE1DEADu || (magic2 != 672274793u && magic2 != 85072278u && magic2 != 369367448u &&
                                  magic2 != 537993216u))
        return -EINVAL;
    switch (cmd) {
    case 0x01234567u: power_reboot();
    case 0x4321FEDCu: power_off();
    case 0xCDEF0123u: power_halt();
    case 0x89ABCDEFu:
    case 0:
        return 0;
    default:
        return -EINVAL;
    }
}

static long sys_kfault(uint64_t op, uint64_t addr)
{
    if (!cred_is_root())
        return -EPERM;
    if (addr >= USER_ADDR_MAX)
        return -EFAULT;
    if (op == 1) {
        if (!smep_enabled)
            return -EOPNOTSUPP;
        ((void (*)(void))addr)();
        return 0;
    }
    if (op == 2) {
        if (!smap_enabled)
            return -EOPNOTSUPP;
        return *(volatile uint8_t *)addr;
    }
    return -EINVAL;
}

static long sys_openpty(int *ufds)
{
    if (!user_range_ok((uint64_t)ufds, 2 * sizeof(int)))
        return -EFAULT;
    struct file *master, *slave;
    int r = pty_open_pair(&master, &slave);
    if (r < 0)
        return r;
    return install_pair(master, slave, ufds, false);
}

static long do_poll(struct user_pollfd *ufds, uint64_t nfds, int timeout, uint64_t deadline);

static long sys_poll(struct user_pollfd *ufds, uint64_t nfds, int timeout)
{
    uint64_t deadline = timeout > 0 ? time_uptime_ms() + (uint64_t)timeout : 0;
    return do_poll(ufds, nfds, timeout, deadline);
}

static long sys_restart_syscall(void)
{
    struct thread *t = thread_current();
    int kind = t->restart_kind;
    t->restart_kind = RESTART_NONE;
    switch (kind) {
    case RESTART_SLEEP:
        return timesys_restart_sleep();
    case RESTART_POLL: {
        uint64_t deadline = t->restart_deadline_us;
        int timeout = deadline > time_uptime_ms() ? 1 : 0;
        return do_poll((struct user_pollfd *)t->restart_arg[0], t->restart_arg[1], timeout, deadline);
    }
    default:
        return -EINTR;
    }
}

static long poll_core(struct user_pollfd *fds, uint64_t nfds, int timeout, uint64_t deadline)
{
    struct poll_table *pt = kzalloc(sizeof(*pt));
    if (!pt)
        return -ENOMEM;
    long ret = 0;
    for (;;) {
        poll_table_init(pt);
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = process_fd_pin(fds[i].fd);
            if (!f) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            short mask = (short)(fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            short got = (short)(file_poll(f, pt) & mask);
            if (got) {
                fds[i].revents = got;
                ready++;
            }
        }
        if (ready) {
            poll_table_clear(pt);
            ret = ready;
            break;
        }
        if (timeout == 0) {
            poll_table_clear(pt);
            break;
        }
        uint64_t wait_ms = 0;
        if (timeout > 0) {
            uint64_t now = time_uptime_ms();
            if (now >= deadline) {
                poll_table_clear(pt);
                break;
            }
            wait_ms = deadline - now;
        }
        poll_table_sleep(pt, wait_ms);
        if (signal_pending_current()) {
            ret = -EINTR;
            break;
        }
    }
    kfree(pt);
    return ret;
}

static long do_poll(struct user_pollfd *ufds, uint64_t nfds, int timeout, uint64_t deadline)
{
    if (nfds > POLL_TABLE_MAX)
        return -EINVAL;
    struct user_pollfd fds[POLL_TABLE_MAX];
    if (copy_from_user(fds, ufds, nfds * sizeof(*fds)) < 0)
        return -EFAULT;
    long ret = poll_core(fds, nfds, timeout, deadline);
    if (ret == -EINTR) {
        if (timeout < 0)
            return -ERESTARTNOHAND;
        struct thread *t = thread_current();
        t->restart_kind = RESTART_POLL;
        t->restart_deadline_us = deadline;
        t->restart_arg[0] = (uint64_t)ufds;
        t->restart_arg[1] = nfds;
        return -ERESTART_RESTARTBLOCK;
    }
    if (ret >= 0 && copy_to_user(ufds, fds, nfds * sizeof(*fds)) < 0)
        return -EFAULT;
    return ret;
}

#define SELECT_MAX 1024
#define POLLPRI_   0x0002

static long sys_select(int nfds, uint64_t *ur, uint64_t *uw, uint64_t *ue, struct user_timeval *utv)
{
    if (nfds < 0 || nfds > SELECT_MAX)
        return -EINVAL;
    uint64_t sets[3][SELECT_MAX / 64];
    uint64_t *usets[3] = { ur, uw, ue };
    size_t bytes = (size_t)(nfds + 63) / 64 * 8;
    memset(sets, 0, sizeof(sets));
    for (int k = 0; k < 3; k++)
        if (usets[k] && bytes && copy_from_user(sets[k], usets[k], bytes) < 0)
            return -EFAULT;
    int timeout = -1;
    uint64_t deadline = 0;
    if (utv) {
        struct user_timeval tv;
        if (copy_from_user(&tv, utv, sizeof(tv)) < 0)
            return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec > 999999)
            return -EINVAL;
        uint64_t ms = (uint64_t)tv.tv_sec * 1000 + ((uint64_t)tv.tv_usec + 999) / 1000;
        if (ms > 0x7FFFFFFF)
            ms = 0x7FFFFFFF;
        timeout = (int)ms;
        deadline = time_uptime_ms() + ms;
    }
    struct user_pollfd fds[POLL_TABLE_MAX];
    uint64_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (sets[0][fd / 64] >> (fd % 64) & 1)
            ev |= POLLIN;
        if (sets[1][fd / 64] >> (fd % 64) & 1)
            ev |= POLLOUT;
        if (sets[2][fd / 64] >> (fd % 64) & 1)
            ev |= POLLPRI_;
        if (!ev)
            continue;
        if (n == POLL_TABLE_MAX)
            return -EINVAL;
        fds[n].fd = fd;
        fds[n].events = ev;
        fds[n].revents = 0;
        n++;
    }
    long ret = poll_core(fds, n, timeout, deadline);
    if (ret == -EINTR)
        return -ERESTARTNOHAND;
    if (ret < 0)
        return ret;
    memset(sets, 0, sizeof(sets));
    long count = 0;
    for (uint64_t i = 0; i < n; i++) {
        short r = fds[i].revents;
        int fd = fds[i].fd;
        if (r & POLLNVAL)
            return -EBADF;
        uint64_t bit = 1ull << (fd % 64);
        if ((fds[i].events & POLLIN) && (r & (POLLIN | POLLHUP | POLLERR))) {
            sets[0][fd / 64] |= bit;
            count++;
        }
        if ((fds[i].events & POLLOUT) && (r & (POLLOUT | POLLERR))) {
            sets[1][fd / 64] |= bit;
            count++;
        }
        if ((fds[i].events & POLLPRI_) && (r & POLLPRI_)) {
            sets[2][fd / 64] |= bit;
            count++;
        }
    }
    for (int k = 0; k < 3; k++)
        if (usets[k] && bytes && copy_to_user(usets[k], sets[k], bytes) < 0)
            return -EFAULT;
    if (utv) {
        uint64_t now = time_uptime_ms();
        uint64_t left = deadline > now ? deadline - now : 0;
        struct user_timeval tv = { (int64_t)(left / 1000), (int64_t)(left % 1000) * 1000 };
        if (copy_to_user(utv, &tv, sizeof(tv)) < 0)
            return -EFAULT;
    }
    return count;
}

static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    struct process *p = process_current();
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if ((int64_t)arg < 0 || (int64_t)arg >= process_fd_limit(p))
            return -EINVAL;
        file_ref(f);
        int nfd = process_fd_alloc(p, f, (int)arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd < 0)
            file_unref(f);
        return nfd;
    }
    case F_GETFD:
        return process_fd_get_flags(p, fd);
    case F_SETFD:
        return process_fd_set_flags(p, fd, (int)arg);
    case F_GETFL:
        return (long)f->flags;
    case F_SETFL:
        f->flags = (f->flags & ~(uint32_t)O_NONBLOCK) | ((uint32_t)arg & O_NONBLOCK);
        return 0;
    default:
        return -EINVAL;
    }
}

static long sys_kill(int pid, int sig)
{
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;
    return process_kill(pid, sig);
}

static long sys_rt_sigaction(int sig, const struct user_sigaction *uact, struct user_sigaction *uold,
                             uint64_t setsize)
{
    if (setsize != sizeof(uint64_t))
        return -EINVAL;
    struct user_sigaction act, old;
    if (uact && copy_from_user(&act, uact, sizeof(act)) < 0)
        return -EFAULT;
    if (uold && !user_range_ok((uint64_t)uold, sizeof(old)))
        return -EFAULT;
    long r = signal_sigaction(sig, uact ? &act : NULL, uold ? &old : NULL);
    if (r == 0 && uold && copy_to_user(uold, &old, sizeof(old)) < 0)
        return -EFAULT;
    return r;
}

static long sys_rt_sigprocmask(int how, const uint64_t *uset, uint64_t *uold, uint64_t setsize)
{
    if (setsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t set, old;
    if (uset && copy_from_user(&set, uset, sizeof(set)) < 0)
        return -EFAULT;
    if (uold && !user_range_ok((uint64_t)uold, sizeof(old)))
        return -EFAULT;
    long r = signal_procmask(how, uset ? &set : NULL, uold ? &old : NULL);
    if (r == 0 && uold && copy_to_user(uold, &old, sizeof(old)) < 0)
        return -EFAULT;
    return r;
}

static void fill_stat(struct vnode *v, uint32_t mode, struct user_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_blksize = 4096;
    if (v) {
        struct vattr va;
        vfs_getattr(v, &va);
        st->st_size = (int64_t)__atomic_load_n(&v->size, __ATOMIC_ACQUIRE);
        st->st_mode = va.mode;
        st->st_ino = v->ino;
        st->st_dev = v->mnt->id;
        st->st_nlink = va.nlink;
        st->st_uid = va.uid;
        st->st_gid = va.gid;
        st->st_blocks = (int64_t)va.blocks;
        st->st_atime = va.atime.sec;
        st->st_atime_nsec = va.atime.nsec;
        st->st_mtime = va.mtime.sec;
        st->st_mtime_nsec = va.mtime.nsec;
        st->st_ctime = va.ctime.sec;
        st->st_ctime_nsec = va.ctime.nsec;
        return;
    }
    st->st_mode = mode ? mode : S_IFIFO | 0600;
    st->st_nlink = 1;
    int64_t t = (int64_t)rtc_boot_epoch();
    st->st_atime = st->st_mtime = st->st_ctime = t;
}

static long sys_newfstatat(int dirfd, const char *upath, struct user_stat *ust, int flags);

static long sys_stat(const char *upath, struct user_stat *ust)
{
    return sys_newfstatat(AT_FDCWD, upath, ust, 0);
}

static long sys_fstat(int fd, struct user_stat *ust)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    struct user_stat st;
    fill_stat(f->vn, f->mode, &st);
    return copy_to_user(ust, &st, sizeof(st));
}

static long sys_newfstatat(int dirfd, const char *upath, struct user_stat *ust, int flags)
{
    if (flags & AT_EMPTY_PATH) {
        char c;
        if (copy_from_user(&c, upath, 1) == 0 && c == 0)
            return sys_fstat(dirfd, ust);
    }
    if (!user_range_ok((uint64_t)ust, sizeof(*ust)))
        return -EFAULT;
    struct upath up;
    struct vnode *v;
    long r = walk_user_ex(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), &v, &up);
    if (r < 0)
        return r;
    struct user_stat st;
    fill_stat(v, 0, &st);
    vnode_put(v);
    return copy_to_user(ust, &st, sizeof(st));
}

static long fd_vnode(int fd, struct vnode **out)
{
    if (fd == AT_FDCWD) {
        *out = process_cwd_vnode(process_current());
        return *out ? 0 : -ENOENT;
    }
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->vn)
        return -EINVAL;
    vnode_get(f->vn);
    *out = f->vn;
    return 0;
}

static long attr_target(int dirfd, const char *upath, int flags, struct vnode **out)
{
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    if (flags & AT_EMPTY_PATH) {
        char c;
        if (copy_from_user(&c, upath, 1) < 0)
            return -EFAULT;
        if (c == 0)
            return fd_vnode(dirfd, out);
    }
    struct upath up;
    return walk_user_ex(dirfd, upath, !(flags & AT_SYMLINK_NOFOLLOW), out, &up);
}

static long owner_or_root(struct vnode *v, struct cred *c, struct vattr *cur)
{
    cred_get(c);
    vfs_getattr(v, cur);
    return (c->euid == 0 || c->euid == cur->uid) ? 0 : -EPERM;
}

static long chmod_vnode(struct vnode *v, uint32_t mode)
{
    long r = -EOPNOTSUPP;
    struct cred c;
    struct vattr cur;
    if (!vnode_is_link(v) && (r = owner_or_root(v, &c, &cur)) == 0) {
        mode &= 07777;
        if (c.euid != 0 && !cred_member(&c, cur.gid))
            mode &= ~(uint32_t)S_ISGID;
        struct vattr va = { .mask = VA_MODE, .mode = mode };
        r = vfs_setattr(v, &va);
    }
    vnode_put(v);
    return r;
}

static long sys_fchmodat2(int dirfd, const char *upath, uint32_t mode, int flags)
{
    struct vnode *v;
    long r = attr_target(dirfd, upath, flags, &v);
    return r < 0 ? r : chmod_vnode(v, mode);
}

static long sys_fchmod(int fd, uint32_t mode)
{
    struct vnode *v;
    long r = fd == AT_FDCWD ? -EBADF : fd_vnode(fd, &v);
    return r < 0 ? r : chmod_vnode(v, mode);
}

static long chown_vnode(struct vnode *v, uint32_t uid, uint32_t gid)
{
    struct vattr va = { 0 };
    if (uid != (uint32_t)-1) {
        va.mask |= VA_UID;
        va.uid = uid;
    }
    if (gid != (uint32_t)-1) {
        va.mask |= VA_GID;
        va.gid = gid;
    }
    if (!vnode_is_dir(v))
        va.mask |= VA_KILL_SUID;
    struct cred c;
    struct vattr cur;
    long r = owner_or_root(v, &c, &cur);
    if (r == 0 && c.euid != 0) {
        if ((va.mask & VA_UID) && va.uid != cur.uid)
            r = -EPERM;
        else if ((va.mask & VA_GID) && va.gid != cur.gid && !cred_member(&c, va.gid))
            r = -EPERM;
    }
    if (r == 0)
        r = vfs_setattr(v, &va);
    vnode_put(v);
    return r;
}

static long sys_fchownat(int dirfd, const char *upath, uint32_t uid, uint32_t gid, int flags)
{
    struct vnode *v;
    long r = attr_target(dirfd, upath, flags, &v);
    return r < 0 ? r : chown_vnode(v, uid, gid);
}

static long sys_fchown(int fd, uint32_t uid, uint32_t gid)
{
    struct vnode *v;
    long r = fd == AT_FDCWD ? -EBADF : fd_vnode(fd, &v);
    return r < 0 ? r : chown_vnode(v, uid, gid);
}

static long set_times(int dirfd, const char *upath, int flags, const struct user_timespec *ts)
{
    struct vattr va = { 0 };
    struct vfs_time now;
    vfs_now(&now);
    for (int i = 0; i < 2; i++) {
        struct vfs_time *dst = i == 0 ? &va.atime : &va.mtime;
        if (ts && ts[i].tv_nsec == UTIME_OMIT)
            continue;
        if (!ts || ts[i].tv_nsec == UTIME_NOW) {
            *dst = now;
        } else {
            if (ts[i].tv_nsec < 0 || ts[i].tv_nsec > 999999999)
                return -EINVAL;
            dst->sec = ts[i].tv_sec;
            dst->nsec = (uint32_t)ts[i].tv_nsec;
        }
        va.mask |= i == 0 ? VA_ATIME : VA_MTIME;
    }
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    struct vnode *v;
    long r;
    if (!upath) {
        if (dirfd == AT_FDCWD)
            return -EFAULT;
        if (flags)
            return -EINVAL;
        r = fd_vnode(dirfd, &v);
    } else {
        r = attr_target(dirfd, upath, flags, &v);
    }
    if (r < 0)
        return r;
    if (va.mask) {
        struct cred c;
        struct vattr cur;
        r = owner_or_root(v, &c, &cur);
        bool to_now = !ts || (ts[0].tv_nsec == UTIME_NOW && ts[1].tv_nsec == UTIME_NOW);
        if (r < 0 && to_now && vfs_permission(v, VFS_MAY_WRITE) == 0)
            r = 0;
        if (r == 0)
            r = vfs_setattr(v, &va);
    }
    vnode_put(v);
    return r;
}

static long sys_utimensat(int dirfd, const char *upath, const struct user_timespec *utimes, int flags)
{
    struct user_timespec ts[2];
    if (utimes && copy_from_user(ts, utimes, sizeof(ts)) < 0)
        return -EFAULT;
    return set_times(dirfd, upath, flags, utimes ? ts : NULL);
}

static long sys_futimesat(int dirfd, const char *upath, const struct user_timeval *utv)
{
    struct user_timeval tv[2];
    struct user_timespec ts[2];
    if (utv) {
        if (copy_from_user(tv, utv, sizeof(tv)) < 0)
            return -EFAULT;
        for (int i = 0; i < 2; i++) {
            if (tv[i].tv_usec < 0 || tv[i].tv_usec > 999999)
                return -EINVAL;
            ts[i].tv_sec = tv[i].tv_sec;
            ts[i].tv_nsec = tv[i].tv_usec * 1000;
        }
    }
    if (!upath)
        return set_times(dirfd, NULL, 0, utv ? ts : NULL);
    return set_times(dirfd, upath, 0, utv ? ts : NULL);
}

static long sys_utime(const char *upath, const int64_t *ubuf)
{
    int64_t b[2];
    struct user_timespec ts[2];
    if (ubuf) {
        if (copy_from_user(b, ubuf, sizeof(b)) < 0)
            return -EFAULT;
        ts[0].tv_sec = b[0];
        ts[1].tv_sec = b[1];
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
    }
    if (!upath)
        return -EFAULT;
    return set_times(AT_FDCWD, upath, 0, ubuf ? ts : NULL);
}

static long faccess(int dirfd, const char *upath, int mode, bool follow, bool effective)
{
    if (mode & ~7)
        return -EINVAL;
    struct upath up;
    struct vnode *v;
    long r = walk_user_ex(dirfd, upath, follow, &v, &up);
    if (r < 0)
        return r;
    if ((mode & 2) && (vnode_is_reg(v) || vnode_is_dir(v)) && vfs_is_rdonly(v))
        r = -EROFS;
    if (r == 0 && mode) {
        struct cred c;
        cred_get(&c);
        r = vfs_permission_as(v, mode, &c, !effective);
    }
    vnode_put(v);
    return r;
}

static long sys_faccessat(int dirfd, const char *upath, int mode)
{
    return faccess(dirfd, upath, mode, true, false);
}

static long sys_faccessat2(int dirfd, const char *upath, int mode, int flags)
{
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return -EINVAL;
    if (flags & AT_EMPTY_PATH) {
        char c;
        if (copy_from_user(&c, upath, 1) < 0)
            return -EFAULT;
        if (c == 0) {
            if (mode & ~7)
                return -EINVAL;
            if (dirfd == AT_FDCWD)
                return 0;
            return process_fd_pin(dirfd) ? 0 : -EBADF;
        }
    }
    return faccess(dirfd, upath, mode, !(flags & AT_SYMLINK_NOFOLLOW), (flags & AT_EACCESS) != 0);
}

static long sys_readlinkat(int dirfd, const char *upath, char *ubuf, uint64_t size)
{
    struct upath up;
    struct vnode *v;
    long r = walk_user_ex(dirfd, upath, false, &v, &up);
    if (r < 0)
        return r;
    if ((v->mode & S_IFMT) != S_IFLNK || !v->mnt->type->readlink) {
        vnode_put(v);
        return -EINVAL;
    }
    if ((int64_t)size <= 0) {
        vnode_put(v);
        return -EINVAL;
    }
    if (size > VFS_PATH_MAX)
        size = VFS_PATH_MAX;
    char *buf = kmalloc(size);
    if (!buf) {
        vnode_put(v);
        return -ENOMEM;
    }
    int64_t n = v->mnt->type->readlink(v, buf, size);
    vnode_put(v);
    if (n >= 0 && copy_to_user(ubuf, buf, (size_t)n) < 0)
        n = -EFAULT;
    kfree(buf);
    return n;
}

static long sys_symlinkat(const char *utarget, int dirfd, const char *upath)
{
    char *target = kmalloc(USER_PATH_MAX);
    if (!target)
        return -ENOMEM;
    long r = strncpy_from_user(target, utarget, USER_PATH_MAX);
    if (r >= 0) {
        struct upath up;
        r = upath_get(dirfd, upath, &up);
        if (r == 0)
            r = vfs_symlink(up.start, up.path, target);
        upath_put(&up);
    }
    kfree(target);
    return r;
}

static uint8_t dirent_type(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return DT_DIR;
    case S_IFCHR: return DT_CHR;
    case S_IFBLK: return DT_BLK;
    case S_IFLNK: return DT_LNK;
    case S_IFIFO: return DT_FIFO;
    case S_IFSOCK: return DT_SOCK;
    default:      return DT_REG;
    }
}

struct dents_ctx {
    uint8_t *buf;
    uint64_t cap;
    uint64_t used;
    bool full;
};

static bool dents_cb(const struct vfs_dirent *e, void *ctx)
{
    struct dents_ctx *c = ctx;
    size_t namelen = strlen(e->name);
    uint16_t reclen = (uint16_t)ALIGN_UP(sizeof(struct linux_dirent64) + namelen + 1, 8);
    if (c->used + reclen > c->cap) {
        c->full = true;
        return false;
    }
    struct linux_dirent64 *d = (struct linux_dirent64 *)(c->buf + c->used);
    memset(d, 0, reclen);
    d->d_ino = e->ino ? e->ino : 1;
    d->d_off = (int64_t)e->next;
    d->d_reclen = reclen;
    d->d_type = dirent_type(e->mode);
    memcpy(d->d_name, e->name, namelen + 1);
    c->used += reclen;
    return true;
}

static long sys_getdents64(int fd, void *ubuf, uint64_t count)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->vn || !vnode_is_dir(f->vn))
        return -ENOTDIR;
    if (!user_range_ok((uint64_t)ubuf, count))
        return -EFAULT;
    struct dents_ctx c = { .cap = count < 32768 ? count : 32768 };
    c.buf = kmalloc(c.cap ? c.cap : 1);
    if (!c.buf)
        return -ENOMEM;
    uint64_t pos = f->offset;
    int r = vfs_readdir_vnode(f->vn, &pos, dents_cb, &c);
    long ret = (long)c.used;
    if (!c.used && r < 0)
        ret = r;
    else if (!c.used && c.full)
        ret = -EINVAL;
    if (ret > 0 && copy_to_user(ubuf, c.buf, c.used) < 0)
        ret = -EFAULT;
    if (ret >= 0)
        f->offset = pos;
    kfree(c.buf);
    return ret;
}

static long sys_lseek(int fd, int64_t off, int whence)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->vn)
        return -ESPIPE;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->offset; break;
    case SEEK_END: base = (int64_t)f->vn->size; break;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->offset = (uint64_t)(base + off);
    return (long)f->offset;
}

static uint32_t prot_to_vma(int prot)
{
    uint32_t f = 0;
    if (prot & PROT_READ)
        f |= VMA_READ;
    if (prot & PROT_WRITE)
        f |= VMA_WRITE | VMA_READ;
    if (prot & PROT_EXEC)
        f |= VMA_EXEC | VMA_READ;
    return f;
}

static long do_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    struct process *p = process_current();
    int type = flags & MAP_TYPE;
    if (len == 0 || !IS_ALIGNED(off, PAGE_SIZE) || (type != MAP_PRIVATE && type != MAP_SHARED))
        return -EINVAL;
    if (len > USER_MMAP_END)
        return -ENOMEM;
    len = PAGE_ALIGN_UP(len);
    bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && (!IS_ALIGNED(addr, PAGE_SIZE) || addr == 0 || addr >= USER_STACK_TOP ||
                  len > USER_STACK_TOP - addr))
        return -EINVAL;

    struct file *f = NULL;
    uint32_t vflags = prot_to_vma(prot) | (type == MAP_SHARED ? VMA_SHARED : 0);
    if (!(flags & MAP_ANONYMOUS)) {
        f = process_fd_pin(fd);
        if (!f)
            return -EBADF;
        if (!f->ops->mmap) {
            if (!file_is_regular(f) || vnode_is_dir(f->vn))
                return -ENODEV;
            if (type == MAP_SHARED && (prot & PROT_WRITE))
                return -ENODEV;
            vflags |= VMA_FILE;
            vflags &= ~(uint32_t)VMA_SHARED;
        }
    }

    uint64_t base;
    if (fixed) {
        if (flags & MAP_FIXED_NOREPLACE) {
            if (!vma_range_free(p, addr, addr + len))
                return -EEXIST;
        } else {
            vma_unmap(p, addr, addr + len);
        }
        base = addr;
    } else if (addr && IS_ALIGNED(addr, PAGE_SIZE) && addr >= USER_MMAP_BASE && addr < USER_MMAP_END &&
               len <= USER_MMAP_END - addr && vma_range_free(p, addr, addr + len)) {
        base = addr;
    } else {
        base = vma_find_free(p, len);
        if (!base)
            return -ENOMEM;
    }

    if (f && f->ops->mmap) {
        int64_t r = file_mmap(f, base, len, prot, off);
        if (r < 0)
            return r;
        vflags |= VMA_SHARED | VMA_NOFREE;
        if (!vma_add(p, base, base + len, vflags, NULL, 0, 0))
            return -ENOMEM;
        return (long)base;
    }
    if (!vma_add(p, base, base + len, vflags, f, off, f ? f->vn->size : 0))
        return -ENOMEM;
    if (!f && (vflags & VMA_SHARED)) {
        uint64_t pte_flags = vma_pte_flags(vflags);
        bool none = !(vflags & (VMA_READ | VMA_WRITE | VMA_EXEC));
        for (uint64_t va = base; va < base + len; va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                vma_unmap(p, base, base + len);
                return -ENOMEM;
            }
            memset(phys_to_virt(phys), 0, PAGE_SIZE);
            if (none)
                vmm_set_pte_in(p->pml4_phys, va, phys | (pte_flags & ~PTE_PRESENT) | PTE_PROTNONE);
            else
                vmm_map_page_in(p->pml4_phys, va, phys, pte_flags);
            p->pages++;
        }
    }
    return (long)base;
}

static long sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = do_mmap(addr, len, prot, flags, fd, off);
    if ((flags & MAP_FIXED) || r < 0 || p->free_n)
        process_mm_tlb_sync(p);
    mutex_unlock(&p->mm_lock);
    return r;
}

static long sys_mremap(uint64_t old, uint64_t old_len, uint64_t new_len, int flags)
{
    if (!IS_ALIGNED(old, PAGE_SIZE) || !old_len || !new_len || (flags & ~MREMAP_MAYMOVE))
        return -EINVAL;
    if (old_len > USER_ADDR_MAX || new_len > USER_ADDR_MAX)
        return -ENOMEM;
    old_len = PAGE_ALIGN_UP(old_len);
    new_len = PAGE_ALIGN_UP(new_len);
    if (!user_range_ok(old, old_len))
        return -EFAULT;
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = vma_remap(p, old, old_len, new_len, (flags & MREMAP_MAYMOVE) != 0);
    if (r >= 0 && ((uint64_t)r != old || new_len < old_len || p->free_n))
        process_mm_tlb_sync(p);
    mutex_unlock(&p->mm_lock);
    return r;
}

static long sys_munmap(uint64_t addr, uint64_t len)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || len == 0 || !user_range_ok(addr, len))
        return -EINVAL;
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = vma_unmap(p, addr, addr + PAGE_ALIGN_UP(len));
    process_mm_tlb_sync(p);
    mutex_unlock(&p->mm_lock);
    return r;
}

static long sys_mprotect(uint64_t addr, uint64_t len, int prot)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || !user_range_ok(addr, len))
        return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if (len == 0)
        return 0;
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = vma_protect(p, addr, addr + PAGE_ALIGN_UP(len), prot_to_vma(prot));
    process_mm_tlb_sync(p);
    mutex_unlock(&p->mm_lock);
    return r;
}

static long sys_madvise(uint64_t addr, uint64_t len, int advice)
{
    if (!IS_ALIGNED(addr, PAGE_SIZE) || !user_range_ok(addr, len))
        return -EINVAL;
    if (advice != MADV_DONTNEED)
        return 0;
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = vma_discard(p, addr, addr + PAGE_ALIGN_UP(len));
    process_mm_tlb_sync(p);
    mutex_unlock(&p->mm_lock);
    return r;
}

static long sys_ioctl(int fd, uint64_t request, uint64_t arg)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    return file_ioctl(f, request, arg);
}

static long sys_dup(int oldfd)
{
    struct process *p = process_current();
    struct file *f = process_fd_pin(oldfd);
    if (!f)
        return -EBADF;
    file_ref(f);
    int fd = process_fd_install(p, f);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static long sys_getcwd(char *ubuf, size_t size)
{
    struct process *p = process_current();
    if (!user_range_ok((uint64_t)ubuf, size))
        return -EFAULT;
    char cwd[PROCESS_CWD_MAX];
    process_get_cwd(p, cwd, sizeof(cwd));
    struct vnode *cv = process_cwd_vnode(p);
    if (!cv)
        return -ENOENT;
    if (cv->unlinked) {
        vnode_put(cv);
        return -ENOENT;
    }
    struct vnode *w = NULL;
    bool fresh = vfs_walk(NULL, cwd, &w) == 0 && w == cv;
    vnode_put(w);
    if (!fresh) {
        long e = vfs_getpath(cv, cwd, sizeof(cwd));
        if (e < 0) {
            vnode_put(cv);
            return e;
        }
        process_update_cwd_path(p, cv, cwd);
    }
    vnode_put(cv);
    size_t n = strlen(cwd) + 1;
    if (n > size)
        return -ERANGE;
    long r = copy_to_user(ubuf, cwd, n);
    return r < 0 ? r : (long)n;
}

static long sys_chdir(const char *upath)
{
    struct process *p = process_current();
    struct upath up;
    long r = upath_get(AT_FDCWD, upath, &up);
    struct vnode *v = NULL;
    bool followed = false;
    if (r == 0)
        r = vfs_walk_ex(up.start, up.path, true, &v, &followed);
    upath_put(&up);
    if (r < 0)
        return r;
    if (followed)
        up.full_ok = false;
    if (!vnode_is_dir(v)) {
        vnode_put(v);
        return -ENOTDIR;
    }
    r = vfs_permission(v, VFS_MAY_EXEC);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    if (!up.full_ok && vfs_getpath(v, up.full, sizeof(up.full)) < 0)
        up.full[0] = '\0';
    process_set_cwd(p, up.full, v);
    return 0;
}

static long sys_fchdir(int fd)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->vn || !vnode_is_dir(f->vn))
        return -ENOTDIR;
    long perm = vfs_permission(f->vn, VFS_MAY_EXEC);
    if (perm < 0)
        return perm;
    char path[PROCESS_CWD_MAX];
    if (vfs_getpath(f->vn, path, sizeof(path)) < 0)
        path[0] = '\0';
    vnode_get(f->vn);
    process_set_cwd(process_current(), path, f->vn);
    return 0;
}

static long copy_type(char *out, size_t size, const char *utype)
{
    long r = strncpy_from_user(out, utype, size);
    if (r < 0)
        return r;
    return strlen(out) >= size - 1 ? -ENODEV : 0;
}

static long sys_mount(const char *usource, const char *utarget, const char *utype, uint64_t flags,
                      const void *data)
{
    if (!cred_is_root())
        return -EPERM;
    if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
        flags &= ~(uint64_t)MS_MGC_MSK;
    uint64_t known = MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_SYNCHRONOUS | MS_SILENT |
                     MS_NOATIME | MS_NODIRATIME | MS_RELATIME;
    if (flags & ~known)
        return -EINVAL;
    char type[16];
    if (!utype)
        return -EINVAL;
    long r = copy_type(type, sizeof(type), utype);
    if (r < 0)
        return r;
    const struct fs_type *t = vfs_find_type(type);
    if (!t)
        return -ENODEV;

    char opts[256];
    opts[0] = 0;
    if (data && (t == vfs_find_type("tmpfs"))) {
        long n = strncpy_from_user(opts, data, sizeof(opts));
        if (n < 0)
            return n == -ENAMETOOLONG ? -EINVAL : -EFAULT;
    }
    struct upath *up = kmalloc(sizeof(*up));
    if (!up)
        return -ENOMEM;
    struct blockdev *dev = NULL;
    char source[VFS_SOURCE_MAX];
    strlcpy(source, t->name, sizeof(source));
    if (t->needs_dev) {
        struct vnode *sv;
        r = usource ? walk_user(AT_FDCWD, usource, &sv, up) : -EFAULT;
        if (r == 0) {
            dev = devfs_vnode_blockdev(sv);
            vnode_put(sv);
            if (!dev)
                r = -ENOTBLK;
            else if (up->full_ok)
                strlcpy(source, up->full, sizeof(source));
        }
    } else if (usource) {
        char tmp[VFS_SOURCE_MAX];
        if (strncpy_from_user(tmp, usource, sizeof(tmp)) >= 0 && tmp[0])
            strlcpy(source, tmp, sizeof(source));
    }
    struct vnode *target = NULL;
    if (r == 0)
        r = walk_user(AT_FDCWD, utarget, &target, up);
    if (r == 0 && !up->full_ok && vfs_getpath(target, up->full, sizeof(up->full)) < 0)
        r = -ENAMETOOLONG;
    if (r == 0) {
        r = vfs_mount(t, dev, source, target, up->full, (flags & MS_RDONLY) ? MNT_RDONLY : 0, opts);
        if (r == 0)
            log_info("vfs", "%s montado em %s (%s%s)", source, up->full, t->name,
                     (flags & MS_RDONLY) ? ", somente leitura" : "");
    }
    vnode_put(target);
    kfree(up);
    return r;
}

static long sys_umount2(const char *utarget, int flags)
{
    if (!cred_is_root())
        return -EPERM;
    if (flags & ~UMOUNT_NOFOLLOW)
        return -EINVAL;
    struct upath *up = kmalloc(sizeof(*up));
    if (!up)
        return -ENOMEM;
    struct vnode *v;
    long r = walk_user_ex(AT_FDCWD, utarget, !(flags & UMOUNT_NOFOLLOW), &v, up);
    kfree(up);
    if (r < 0)
        return r;
    return vfs_umount(v);
}

static long sys_arch_prctl(int code, uint64_t addr)
{
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_ADDR_MAX)
            return -EFAULT;
        thread_current()->fs_base = addr;
        wrmsr(MSR_IA32_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS: {
        uint64_t v = thread_current()->fs_base;
        return copy_to_user((void *)addr, &v, sizeof(v));
    }
    default:
        return -EINVAL;
    }
}

static long sys_dup3(int oldfd, int newfd, int flags)
{
    struct process *p = process_current();
    if (flags & ~O_CLOEXEC)
        return -EINVAL;
    struct file *f = process_fd_pin(oldfd);
    if (!f)
        return -EBADF;
    if (oldfd == newfd)
        return -EINVAL;
    file_ref(f);
    long r = process_fd_replace(p, newfd, f, (flags & O_CLOEXEC) != 0);
    if (r < 0)
        file_unref(f);
    return r;
}

static long sys_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return process_fd_pin(oldfd) ? newfd : -EBADF;
    return sys_dup3(oldfd, newfd, 0);
}

static long do_brk(struct process *p, uint64_t addr)
{
    if (addr == 0 || addr < p->brk_start || addr > p->brk_start + USER_HEAP_MAX)
        return (long)p->brk;
    uint64_t old_end = PAGE_ALIGN_UP(p->brk);
    uint64_t new_end = PAGE_ALIGN_UP(addr);
    if (new_end > old_end) {
        if (!vma_range_free(p, old_end, new_end))
            return (long)p->brk;
        struct vma *v = old_end > p->brk_start ? vma_find(p, old_end - 1) : NULL;
        if (v && (v->flags & VMA_HEAP) && v->end == old_end) {
            if (!vma_resize(p, v, new_end))
                return (long)p->brk;
        } else if (!vma_add(p, old_end, new_end, VMA_READ | VMA_WRITE | VMA_HEAP, NULL, 0, 0)) {
            return (long)p->brk;
        }
    } else if (new_end < old_end) {
        vma_unmap(p, new_end, old_end);
        process_mm_tlb_sync(p);
    }
    p->brk = addr;
    return (long)addr;
}

static long sys_brk(uint64_t addr)
{
    struct process *p = process_current();
    mutex_lock(&p->mm_lock);
    long r = do_brk(p, addr);
    mutex_unlock(&p->mm_lock);
    return r;
}

static void free_strv(char **v)
{
    if (!v)
        return;
    for (int i = 0; v[i]; i++)
        kfree(v[i]);
    kfree(v);
}

static long copy_strv(char *const *uv, char ***out)
{
    char **v = kzalloc((EXEC_MAX_ARGS + 1) * sizeof(char *));
    char *tmp = kmalloc(EXEC_ARG_MAX);
    if (!v || !tmp) {
        kfree(v);
        kfree(tmp);
        return -ENOMEM;
    }
    long ret = 0;
    for (int i = 0; uv; i++) {
        if (i >= EXEC_MAX_ARGS) {
            ret = -E2BIG;
            break;
        }
        uint64_t up;
        if (copy_from_user(&up, &uv[i], sizeof(up)) < 0) {
            ret = -EFAULT;
            break;
        }
        if (!up)
            break;
        long n = strncpy_from_user(tmp, (const char *)up, EXEC_ARG_MAX);
        if (n < 0) {
            ret = n == -ENAMETOOLONG ? -E2BIG : n;
            break;
        }
        v[i] = kmalloc((size_t)n + 1);
        if (!v[i]) {
            ret = -ENOMEM;
            break;
        }
        memcpy(v[i], tmp, (size_t)n + 1);
    }
    kfree(tmp);
    if (ret < 0) {
        free_strv(v);
        return ret;
    }
    *out = v;
    return 0;
}

static long sys_execve(const char *upath, char *const *uargv, char *const *uenvp,
                       struct interrupt_frame *f)
{
    char path[USER_PATH_MAX];
    long r = strncpy_from_user(path, upath, sizeof(path));
    if (r < 0)
        return r;
    if (!path[0])
        return -ENOENT;
    char **argv = NULL, **envp = NULL;
    r = copy_strv(uargv, &argv);
    if (r < 0)
        return r;
    r = copy_strv(uenvp, &envp);
    if (r < 0) {
        free_strv(argv);
        return r;
    }
    r = process_execve(path, argv, envp, f);
    free_strv(argv);
    free_strv(envp);
    return r;
}

static long sys_wait4(int pid, int *ustatus, int options, struct user_rusage *uru)
{
    if (ustatus && !user_range_ok((uint64_t)ustatus, sizeof(int)))
        return -EFAULT;
    if (uru && !user_range_ok((uint64_t)uru, sizeof(*uru)))
        return -EFAULT;
    int st = 0;
    struct cpu_times child = { 0, 0 };
    int r = process_waitpid(pid, &st, options, &child);
    if (r > 0 && ustatus && copy_to_user(ustatus, &st, sizeof(st)) < 0)
        return -EFAULT;
    if (r > 0 && uru && rusage_to_user(&child, uru) < 0)
        return -EFAULT;
    return r;
}

struct index_ctx {
    uint64_t want;
    uint64_t seen;
    struct vfs_dirent *out;
    bool found;
};

static bool index_cb(const struct vfs_dirent *e, void *ctx)
{
    struct index_ctx *r = ctx;
    if (r->seen++ != r->want)
        return true;
    *r->out = *e;
    r->found = true;
    return false;
}

static long sys_readdir(int fd, struct user_dirent *uout, uint64_t index)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->vn || !vnode_is_dir(f->vn))
        return -ENOTDIR;
    if (!user_range_ok((uint64_t)uout, sizeof(*uout)))
        return -EFAULT;
    struct vfs_dirent *e = kmalloc(sizeof(*e));
    if (!e)
        return -ENOMEM;
    struct index_ctx ic = { .want = index, .out = e };
    uint64_t pos = 0;
    int r = vfs_readdir_vnode(f->vn, &pos, index_cb, &ic);
    if (r < 0 || !ic.found) {
        kfree(e);
        return r < 0 ? r : 0;
    }
    struct user_dirent d;
    memset(&d, 0, sizeof(d));
    d.size = e->size;
    d.is_dir = (e->mode & S_IFMT) == S_IFDIR;
    strlcpy(d.name, e->name, sizeof(d.name));
    kfree(e);
    if (copy_to_user(uout, &d, sizeof(d)) < 0)
        return -EFAULT;
    return 1;
}

static long sys_getrlimit(int resource, struct user_rlimit *uout)
{
    struct user_rlimit r;
    long e = process_getrlimit(resource, &r);
    if (e < 0)
        return e;
    return copy_to_user(uout, &r, sizeof(r));
}

static long sys_setrlimit(int resource, const struct user_rlimit *uin)
{
    struct user_rlimit r;
    if (copy_from_user(&r, uin, sizeof(r)) < 0)
        return -EFAULT;
    return process_setrlimit(resource, &r);
}

static long sys_prlimit64(int pid, int resource, const struct user_rlimit *uin, struct user_rlimit *uout)
{
    if (pid != 0 && pid != (int)process_current()->pid)
        return -ESRCH;
    struct user_rlimit old;
    long e = process_getrlimit(resource, &old);
    if (e < 0)
        return e;
    if (uin) {
        e = sys_setrlimit(resource, uin);
        if (e < 0)
            return e;
    }
    if (uout)
        return copy_to_user(uout, &old, sizeof(old));
    return 0;
}

#define IOV_MAX 1024

struct user_iovec {
    uint64_t base;
    uint64_t len;
};

static long sys_rwv(int fd, const struct user_iovec *uiov, int cnt, bool write)
{
    if (cnt < 0 || cnt > IOV_MAX)
        return -EINVAL;
    struct file *f = NULL;
    bool locked = false;
    if (write) {
        f = process_fd_pin(fd);
        if (!f)
            return -EBADF;
        locked = append_lock(f);
    }
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        struct user_iovec v;
        if (copy_from_user(&v, &uiov[i], sizeof(v)) < 0) {
            if (!total)
                total = -EFAULT;
            break;
        }
        if (v.len == 0)
            continue;
        long n = write ? write_file(f, (const void *)v.base, v.len) : sys_read(fd, (void *)v.base, v.len);
        if (n < 0) {
            if (!total)
                total = n;
            break;
        }
        total += n;
        if ((uint64_t)n < v.len)
            break;
    }
    if (locked)
        mutex_unlock(&f->vn->io_lock);
    return total;
}

static long sys_readv(int fd, const struct user_iovec *uiov, int cnt)
{
    return sys_rwv(fd, uiov, cnt, false);
}

static long sys_writev(int fd, const struct user_iovec *uiov, int cnt)
{
    return sys_rwv(fd, uiov, cnt, true);
}

struct user_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static long sys_uname(struct user_utsname *uout)
{
    struct user_utsname u;
    memset(&u, 0, sizeof(u));
    strlcpy(u.sysname, KERNEL_NAME, sizeof(u.sysname));
    strlcpy(u.nodename, "orion", sizeof(u.nodename));
    strlcpy(u.release, KERNEL_VERSION, sizeof(u.release));
    strlcpy(u.version, "#1 SMP " __DATE__ " " __TIME__, sizeof(u.version));
    strlcpy(u.machine, KERNEL_ARCH, sizeof(u.machine));
    strlcpy(u.domainname, "(none)", sizeof(u.domainname));
    return copy_to_user(uout, &u, sizeof(u));
}

static long sys_sched_getaffinity(int pid, uint64_t size, void *umask)
{
    if (pid != 0 && pid != (int)process_current()->pid)
        return -ESRCH;
    if (size < sizeof(uint64_t))
        return -EINVAL;
    uint32_t n = smp_cpu_count();
    uint64_t mask = n >= 64 ? ~0ull : ((1ull << n) - 1);
    if (copy_to_user(umask, &mask, sizeof(mask)) < 0)
        return -EFAULT;
    return sizeof(mask);
}

static long sys_pause(void)
{
    static spinlock_t lock = SPINLOCK_INIT;
    struct waitqueue wq = WAITQUEUE_INIT;
    uint64_t flags = spin_lock_irqsave(&lock);
    while (!signal_pending_current())
        wait_sleep_intr(&wq, &lock, 0);
    spin_unlock_irqrestore(&lock, flags);
    return -ERESTARTNOHAND;
}

static long sys_rt_sigpending(uint64_t *uset, uint64_t setsize)
{
    if (setsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t set = signal_pending_set();
    return copy_to_user(uset, &set, sizeof(set));
}

static long sys_rt_sigsuspend(const uint64_t *uset, uint64_t setsize)
{
    if (setsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t set;
    if (copy_from_user(&set, uset, sizeof(set)) < 0)
        return -EFAULT;
    return signal_suspend(set);
}

static long sys_rt_sigtimedwait(const uint64_t *uset, struct user_siginfo *uinfo, const struct user_timespec *uto,
                                uint64_t setsize)
{
    if (setsize != sizeof(uint64_t))
        return -EINVAL;
    uint64_t set;
    if (copy_from_user(&set, uset, sizeof(set)) < 0)
        return -EFAULT;
    bool has_timeout = false;
    uint64_t ms = 0;
    if (uto) {
        struct user_timespec ts;
        if (copy_from_user(&ts, uto, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
            return -EINVAL;
        if (ts.tv_sec < 1000000000) {
            has_timeout = true;
            ms = (uint64_t)ts.tv_sec * 1000 + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
        }
    }
    if (uinfo && !user_range_ok((uint64_t)uinfo, sizeof(*uinfo)))
        return -EFAULT;
    struct user_siginfo info;
    long r = signal_timedwait(set, &info, has_timeout, ms);
    if (r > 0 && uinfo && copy_to_user(uinfo, &info, sizeof(info)) < 0)
        return -EFAULT;
    return r;
}

#define SS_DISABLE 2

struct user_stack_t {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

static long sys_sigaltstack(const struct user_stack_t *uss, struct user_stack_t *uold)
{
    if (uss) {
        struct user_stack_t ss;
        if (copy_from_user(&ss, uss, sizeof(ss)) < 0)
            return -EFAULT;
    }
    if (uold) {
        struct user_stack_t old;
        memset(&old, 0, sizeof(old));
        old.ss_flags = SS_DISABLE;
        return copy_to_user(uold, &old, sizeof(old));
    }
    return 0;
}

bool syscall_dispatch(struct interrupt_frame *f)
{
    sti();
    long ret;
    bool full_restore = false;
    switch (f->rax) {
    case SYS_READ:        ret = sys_read((int)f->rdi, (void *)f->rsi, (size_t)f->rdx); break;
    case SYS_WRITE:       ret = sys_write((int)f->rdi, (const void *)f->rsi, (size_t)f->rdx); break;
    case SYS_PREAD64:     ret = sys_pread64((int)f->rdi, (void *)f->rsi, (size_t)f->rdx, (int64_t)f->r10); break;
    case SYS_PWRITE64:    ret = sys_pwrite64((int)f->rdi, (const void *)f->rsi, (size_t)f->rdx, (int64_t)f->r10); break;
    case SYS_OPEN:        ret = sys_open((const char *)f->rdi, (int)f->rsi, (uint32_t)f->rdx); break;
    case SYS_CLOSE:       ret = sys_close((int)f->rdi); break;
    case SYS_STAT:        ret = sys_stat((const char *)f->rdi, (struct user_stat *)f->rsi); break;
    case SYS_FSTAT:       ret = sys_fstat((int)f->rdi, (struct user_stat *)f->rsi); break;
    case SYS_LSEEK:       ret = sys_lseek((int)f->rdi, (int64_t)f->rsi, (int)f->rdx); break;
    case SYS_MMAP:        ret = sys_mmap(f->rdi, f->rsi, (int)f->rdx, (int)f->r10, (int)f->r8, f->r9); break;
    case SYS_MUNMAP:      ret = sys_munmap(f->rdi, f->rsi); break;
    case SYS_MPROTECT:    ret = sys_mprotect(f->rdi, f->rsi, (int)f->rdx); break;
    case SYS_MADVISE:     ret = sys_madvise(f->rdi, f->rsi, (int)f->rdx); break;
    case SYS_IOCTL:       ret = sys_ioctl((int)f->rdi, f->rsi, f->rdx); break;
    case SYS_DUP:         ret = sys_dup((int)f->rdi); break;
    case SYS_GETCWD:      ret = sys_getcwd((char *)f->rdi, (size_t)f->rsi); break;
    case SYS_CHDIR:       ret = sys_chdir((const char *)f->rdi); break;
    case SYS_FCHDIR:      ret = sys_fchdir((int)f->rdi); break;
    case SYS_TRUNCATE:    ret = sys_truncate((const char *)f->rdi, f->rsi); break;
    case SYS_MOUNT:       ret = sys_mount((const char *)f->rdi, (const char *)f->rsi, (const char *)f->rdx,
                                          f->r10, (const void *)f->r8); break;
    case SYS_UMOUNT2:     ret = sys_umount2((const char *)f->rdi, (int)f->rsi); break;
    case SYS_GETPPID:     ret = process_getppid(); break;
    case SYS_ARCH_PRCTL:  ret = sys_arch_prctl((int)f->rdi, f->rsi); break;
    case SYS_CLOCK_GETTIME: ret = sys_clock_gettime((int)f->rdi, (struct user_timespec *)f->rsi); break;
    case SYS_BRK:         ret = sys_brk(f->rdi); break;
    case SYS_PIPE:        ret = sys_pipe((int *)f->rdi); break;
    case SYS_SCHED_YIELD: thread_yield(); ret = 0; break;
    case SYS_DUP2:        ret = sys_dup2((int)f->rdi, (int)f->rsi); break;
    case SYS_NANOSLEEP:   ret = sys_nanosleep((const struct user_timespec *)f->rdi, (struct user_timespec *)f->rsi); break;
    case SYS_CLOCK_NANOSLEEP: ret = sys_clock_nanosleep((int)f->rdi, (int)f->rsi, (const struct user_timespec *)f->rdx,
                                                       (struct user_timespec *)f->r10); break;
    case SYS_CLOCK_GETRES: ret = sys_clock_getres((int)f->rdi, (struct user_timespec *)f->rsi); break;
    case SYS_GETTIMEOFDAY: ret = sys_gettimeofday((struct user_timeval *)f->rdi, (void *)f->rsi); break;
    case SYS_TIME:        ret = sys_time((int64_t *)f->rdi); break;
    case SYS_GETRUSAGE:   ret = sys_getrusage((int)f->rdi, (struct user_rusage *)f->rsi); break;
    case SYS_TIMES:       ret = sys_times((struct user_tms *)f->rdi); break;
    case SYS_GETITIMER:   ret = sys_getitimer((int)f->rdi, (struct user_itimerval *)f->rsi); break;
    case SYS_SETITIMER:   ret = sys_setitimer((int)f->rdi, (const struct user_itimerval *)f->rsi,
                                              (struct user_itimerval *)f->rdx); break;
    case SYS_RT_SIGPENDING: ret = sys_rt_sigpending((uint64_t *)f->rdi, f->rsi); break;
    case SYS_RT_SIGSUSPEND: ret = sys_rt_sigsuspend((const uint64_t *)f->rdi, f->rsi); break;
    case SYS_RT_SIGTIMEDWAIT: ret = sys_rt_sigtimedwait((const uint64_t *)f->rdi, (struct user_siginfo *)f->rsi,
                                                        (const struct user_timespec *)f->rdx, f->r10); break;
    case SYS_RESTART_SYSCALL: ret = sys_restart_syscall(); break;
    case SYS_TKILL:       ret = process_signal_thread(0, (int)f->rdi, (int)f->rsi); break;
    case SYS_TGKILL:      ret = (int)f->rdi <= 0 ? -EINVAL : process_signal_thread((int)f->rdi, (int)f->rsi, (int)f->rdx); break;
    case SYS_GETPID:      ret = process_current()->pid; break;
    case SYS_FORK:        ret = process_fork(f); break;
    case SYS_EXECVE:      ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)f->rdx, f); break;
    case SYS_EXIT:        process_exit_thread((int)f->rdi);
    case SYS_WAIT4:       ret = sys_wait4((int)f->rdi, (int *)f->rsi, (int)f->rdx, (struct user_rusage *)f->r10); break;
    case SYS_READDIR:     ret = sys_readdir((int)f->rdi, (struct user_dirent *)f->rsi, f->rdx); break;
    case SYS_OPENPTY:     ret = sys_openpty((int *)f->rdi); break;
    case SYS_KFAULT:      ret = sys_kfault(f->rdi, f->rsi); break;
    case SYS_POLL:        ret = sys_poll((struct user_pollfd *)f->rdi, f->rsi, (int)f->rdx); break;
    case SYS_SELECT:      ret = sys_select((int)f->rdi, (uint64_t *)f->rsi, (uint64_t *)f->rdx, (uint64_t *)f->r10,
                                           (struct user_timeval *)f->r8); break;
    case SYS_LISTEN:      ret = sys_listen((int)f->rdi, (int)f->rsi); break;
    case SYS_ACCEPT:      ret = sys_accept4((int)f->rdi, (struct sockaddr_in *)f->rsi, (uint32_t *)f->rdx, 0); break;
    case SYS_ACCEPT4:     ret = sys_accept4((int)f->rdi, (struct sockaddr_in *)f->rsi, (uint32_t *)f->rdx, (int)f->r10);
                          break;
    case SYS_SHUTDOWN:    ret = sys_shutdown((int)f->rdi, (int)f->rsi); break;
    case SYS_GETSOCKNAME: ret = sys_getsockname((int)f->rdi, (struct sockaddr_in *)f->rsi, (uint32_t *)f->rdx); break;
    case SYS_GETPEERNAME: ret = sys_getpeername((int)f->rdi, (struct sockaddr_in *)f->rsi, (uint32_t *)f->rdx); break;
    case SYS_SETSOCKOPT:  ret = sys_setsockopt((int)f->rdi, (int)f->rsi, (int)f->rdx, (const void *)f->r10,
                                               (uint32_t)f->r8); break;
    case SYS_GETSOCKOPT:  ret = sys_getsockopt((int)f->rdi, (int)f->rsi, (int)f->rdx, (void *)f->r10,
                                               (uint32_t *)f->r8); break;
    case SYS_FCNTL:       ret = sys_fcntl((int)f->rdi, (int)f->rsi, f->rdx); break;
    case SYS_KILL:        ret = sys_kill((int)f->rdi, (int)f->rsi); break;
    case SYS_RT_SIGACTION: ret = sys_rt_sigaction((int)f->rdi, (const struct user_sigaction *)f->rsi, (struct user_sigaction *)f->rdx, f->r10); break;
    case SYS_RT_SIGPROCMASK: ret = sys_rt_sigprocmask((int)f->rdi, (const uint64_t *)f->rsi, (uint64_t *)f->rdx, f->r10); break;
    case SYS_LSTAT:       ret = sys_newfstatat(AT_FDCWD, (const char *)f->rdi, (struct user_stat *)f->rsi,
                                           AT_SYMLINK_NOFOLLOW); break;
    case SYS_SYMLINK:     ret = sys_symlinkat((const char *)f->rdi, AT_FDCWD, (const char *)f->rsi); break;
    case SYS_SYMLINKAT:   ret = sys_symlinkat((const char *)f->rdi, (int)f->rsi, (const char *)f->rdx); break;
    case SYS_ACCESS:      ret = sys_faccessat(AT_FDCWD, (const char *)f->rdi, (int)f->rsi); break;
    case SYS_READLINK:    ret = sys_readlinkat(AT_FDCWD, (const char *)f->rdi, (char *)f->rsi, f->rdx); break;
    case SYS_GETDENTS64:  ret = sys_getdents64((int)f->rdi, (void *)f->rsi, f->rdx); break;
    case SYS_OPENAT:      ret = sys_openat((int)f->rdi, (const char *)f->rsi, (int)f->rdx, (uint32_t)f->r10); break;
    case SYS_MKDIRAT:     ret = sys_mkdirat((int)f->rdi, (const char *)f->rsi, (uint32_t)f->rdx); break;
    case SYS_CHMOD:       ret = sys_fchmodat2(AT_FDCWD, (const char *)f->rdi, (uint32_t)f->rsi, 0); break;
    case SYS_FCHMOD:      ret = sys_fchmod((int)f->rdi, (uint32_t)f->rsi); break;
    case SYS_FCHMODAT:    ret = sys_fchmodat2((int)f->rdi, (const char *)f->rsi, (uint32_t)f->rdx, 0); break;
    case SYS_FCHMODAT2:   ret = sys_fchmodat2((int)f->rdi, (const char *)f->rsi, (uint32_t)f->rdx, (int)f->r10); break;
    case SYS_CHOWN:       ret = sys_fchownat(AT_FDCWD, (const char *)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx, 0); break;
    case SYS_LCHOWN:      ret = sys_fchownat(AT_FDCWD, (const char *)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx,
                                             AT_SYMLINK_NOFOLLOW); break;
    case SYS_FCHOWN:      ret = sys_fchown((int)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); break;
    case SYS_FCHOWNAT:    ret = sys_fchownat((int)f->rdi, (const char *)f->rsi, (uint32_t)f->rdx, (uint32_t)f->r10,
                                             (int)f->r8); break;
    case SYS_UMASK:       ret = sys_umask((uint32_t)f->rdi); break;
    case SYS_UTIME:       ret = sys_utime((const char *)f->rdi, (const int64_t *)f->rsi); break;
    case SYS_UTIMES:      ret = sys_futimesat(AT_FDCWD, (const char *)f->rdi, (const struct user_timeval *)f->rsi); break;
    case SYS_FUTIMESAT:   ret = sys_futimesat((int)f->rdi, (const char *)f->rsi, (const struct user_timeval *)f->rdx);
                          break;
    case SYS_UTIMENSAT:   ret = sys_utimensat((int)f->rdi, (const char *)f->rsi, (const struct user_timespec *)f->rdx,
                                              (int)f->r10); break;
    case SYS_NEWFSTATAT:  ret = sys_newfstatat((int)f->rdi, (const char *)f->rsi, (struct user_stat *)f->rdx, (int)f->r10); break;
    case SYS_UNLINKAT:    ret = sys_unlinkat((int)f->rdi, (const char *)f->rsi, (f->rdx & AT_REMOVEDIR) != 0); break;
    case SYS_RENAMEAT:    ret = sys_renameat((int)f->rdi, (const char *)f->rsi, (int)f->rdx, (const char *)f->r10); break;
    case SYS_READLINKAT:  ret = sys_readlinkat((int)f->rdi, (const char *)f->rsi, (char *)f->rdx, f->r10); break;
    case SYS_FACCESSAT:   ret = sys_faccessat((int)f->rdi, (const char *)f->rsi, (int)f->rdx); break;
    case SYS_MREMAP:      ret = sys_mremap(f->rdi, f->rsi, f->rdx, (int)f->r10); break;
    case SYS_FACCESSAT2:  ret = sys_faccessat2((int)f->rdi, (const char *)f->rsi, (int)f->rdx, (int)f->r10); break;
    case SYS_DUP3:        ret = sys_dup3((int)f->rdi, (int)f->rsi, (int)f->rdx); break;
    case SYS_PIPE2:       ret = sys_pipe2((int *)f->rdi, (int)f->rsi); break;
    case SYS_READV:       ret = sys_readv((int)f->rdi, (const struct user_iovec *)f->rsi, (int)f->rdx); break;
    case SYS_WRITEV:      ret = sys_writev((int)f->rdi, (const struct user_iovec *)f->rsi, (int)f->rdx); break;
    case SYS_UNAME:       ret = sys_uname((struct user_utsname *)f->rdi); break;
    case SYS_GETUID:      ret = sys_getuid(); break;
    case SYS_GETGID:      ret = sys_getgid(); break;
    case SYS_GETEUID:     ret = sys_geteuid(); break;
    case SYS_GETEGID:     ret = sys_getegid(); break;
    case SYS_SETUID:      ret = sys_setuid((uint32_t)f->rdi); break;
    case SYS_SETGID:      ret = sys_setgid((uint32_t)f->rdi); break;
    case SYS_SETREUID:    ret = sys_setreuid((uint32_t)f->rdi, (uint32_t)f->rsi); break;
    case SYS_SETREGID:    ret = sys_setregid((uint32_t)f->rdi, (uint32_t)f->rsi); break;
    case SYS_SETRESUID:   ret = sys_setresuid((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); break;
    case SYS_SETRESGID:   ret = sys_setresgid((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); break;
    case SYS_GETRESUID:   ret = sys_getresuid((uint32_t *)f->rdi, (uint32_t *)f->rsi, (uint32_t *)f->rdx); break;
    case SYS_GETRESGID:   ret = sys_getresgid((uint32_t *)f->rdi, (uint32_t *)f->rsi, (uint32_t *)f->rdx); break;
    case SYS_GETGROUPS:   ret = sys_getgroups((int)f->rdi, (uint32_t *)f->rsi); break;
    case SYS_SETGROUPS:   ret = sys_setgroups(f->rdi, (const uint32_t *)f->rsi); break;
    case SYS_GETTID:      ret = thread_current()->utid; break;
    case SYS_SET_TID_ADDRESS: thread_current()->clear_child_tid = f->rdi; ret = thread_current()->utid; break;
    case SYS_CLONE:       ret = process_clone(f, f->rdi, f->rsi, f->rdx, f->r10, f->r8); break;
    case SYS_VFORK:       ret = process_fork(f); break;
    case SYS_PAUSE:       ret = sys_pause(); break;
    case SYS_EXIT_GROUP:  process_exit((int)f->rdi);
    case SYS_FUTEX:       ret = futex_syscall((uint32_t *)f->rdi, (int)f->rsi, (uint32_t)f->rdx, f->r10,
                                          (uint32_t *)f->r8, (uint32_t)f->r9); break;
    case SYS_SCHED_GETAFFINITY: ret = sys_sched_getaffinity((int)f->rdi, f->rsi, (void *)f->rdx); break;
    case SYS_SIGALTSTACK: ret = sys_sigaltstack((const struct user_stack_t *)f->rdi, (struct user_stack_t *)f->rsi); break;
    case SYS_RT_SIGRETURN: ret = signal_sigreturn(f); full_restore = true; break;
    case SYS_ALARM:       ret = process_alarm(f->rdi); break;
    case SYS_SETPGID:     ret = process_setpgid((int)f->rdi, (int)f->rsi); break;
    case SYS_MKDIR:       ret = sys_mkdir((const char *)f->rdi, (uint32_t)f->rsi); break;
    case SYS_RMDIR:       ret = sys_unlink((const char *)f->rdi, true); break;
    case SYS_UNLINK:      ret = sys_unlink((const char *)f->rdi, false); break;
    case SYS_RENAME:      ret = sys_rename((const char *)f->rdi, (const char *)f->rsi); break;
    case SYS_FTRUNCATE:   ret = sys_ftruncate((int)f->rdi, f->rsi); break;
    case SYS_FSYNC:
    case SYS_SYNC:        ret = vfs_sync() ? 0 : -EIO; break;
    case SYS_REBOOT:      ret = sys_reboot((uint32_t)f->rdi, (uint32_t)f->rsi, (uint32_t)f->rdx); break;
    case SYS_GETPGID:     ret = process_getpgid((int)f->rdi); break;
    case SYS_GETRLIMIT:   ret = sys_getrlimit((int)f->rdi, (struct user_rlimit *)f->rsi); break;
    case SYS_SETRLIMIT:   ret = sys_setrlimit((int)f->rdi, (const struct user_rlimit *)f->rsi); break;
    case SYS_PRLIMIT64:   ret = sys_prlimit64((int)f->rdi, (int)f->rsi, (const struct user_rlimit *)f->rdx, (struct user_rlimit *)f->r10); break;
    case SYS_SOCKET:      ret = sys_socket((int)f->rdi, (int)f->rsi, (int)f->rdx); break;
    case SYS_SOCKETPAIR:  ret = sys_socketpair((int)f->rdi, (int)f->rsi, (int)f->rdx, (int *)f->r10); break;
    case SYS_BIND:        ret = sys_bind((int)f->rdi, (const struct sockaddr_in *)f->rsi, (uint32_t)f->rdx); break;
    case SYS_CONNECT:     ret = sys_connect((int)f->rdi, (const struct sockaddr_in *)f->rsi, (uint32_t)f->rdx); break;
    case SYS_SENDTO:      ret = sys_sendto((int)f->rdi, (const void *)f->rsi, f->rdx, (int)f->r10, (const struct sockaddr_in *)f->r8, (uint32_t)f->r9); break;
    case SYS_SENDMSG:     ret = sys_sendmsg((int)f->rdi, (const struct user_msghdr *)f->rsi, (int)f->rdx); break;
    case SYS_RECVMSG:     ret = sys_recvmsg((int)f->rdi, (struct user_msghdr *)f->rsi, (int)f->rdx); break;
    case SYS_RECVFROM:    ret = sys_recvfrom((int)f->rdi, (void *)f->rsi, f->rdx, (int)f->r10, (struct sockaddr_in *)f->r8, (uint32_t *)f->r9); break;
    default:
        log_warn("syscall", "pid %u: syscall %lu desconhecida", process_current()->pid, f->rax);
        ret = -ENOSYS;
        break;
    }
    f->rax = (uint64_t)ret;
    process_unpin_all();
    signal_check(f);
    return full_restore;
}

void syscall_init_cpu(void)
{
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_IA32_FMASK, RFLAGS_IF | (1 << 8) | (1 << 10) | (1 << 14) | (1 << 18));
    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | EFER_SCE);
}

void syscall_init(void)
{
    syscall_init_cpu();
    log_info("syscall", "syscall/sysret ativos, entrada em %p", (void *)syscall_entry);
}
