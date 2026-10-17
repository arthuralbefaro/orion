#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/file.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
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

static long resolve_at(int dirfd, const char *upath, char *full, size_t size)
{
    char kpath[USER_PATH_MAX];
    long r = strncpy_from_user(kpath, upath, sizeof(kpath));
    if (r < 0)
        return r;
    struct process *p = process_current();
    if (kpath[0] == '/' || dirfd == AT_FDCWD) {
        if (!kpath[0])
            return -ENOENT;
        return process_resolve_path(p, kpath, full, size) ? 0 : -ENAMETOOLONG;
    }
    struct file *dir = process_fd_pin(dirfd);
    if (!dir)
        return -EBADF;
    if (!dir->path || !dir->has_node || !dir->node.is_dir)
        return -ENOTDIR;
    char joined[USER_PATH_MAX + PROCESS_CWD_MAX + 2];
    ksnprintf(joined, sizeof(joined), "%s/%s", dir->path, kpath);
    return process_resolve_path(p, joined, full, size) ? 0 : -ENAMETOOLONG;
}

static long resolve_user(const char *upath, char *full, size_t size)
{
    return resolve_at(AT_FDCWD, upath, full, size);
}

static char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static bool path_lookup(const char *full, struct vfs_node *n, uint32_t *mode)
{
    if (devfs_owns(full)) {
        uint32_t m = devfs_mode(full);
        if (!m)
            return false;
        if (n) {
            memset(n, 0, sizeof(*n));
            n->is_dir = (m & S_IFMT) == S_IFDIR;
        }
        if (mode)
            *mode = m;
        return true;
    }
    struct vfs_node tmp;
    if (!vfs_lookup(full, n ? n : &tmp))
        return false;
    if (mode)
        *mode = 0;
    return true;
}

static long sys_sysinfo(struct user_sysinfo *uout)
{
    struct user_sysinfo out;
    memset(&out, 0, sizeof(out));
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t faults, copies;
    vmm_cow_stats(&faults, &copies);
    out.total_pages = st.total_pages;
    out.used_pages = st.used_pages;
    out.free_pages = st.free_pages;
    out.shared_pages = pmm_shared_pages();
    out.cow_faults = faults;
    out.uptime_ms = time_uptime_ms();
    struct heap_stats hs;
    heap_get_stats(&hs);
    out.heap_used = hs.slab_bytes_used + hs.large_bytes_used;
    out.cpus = smp_cpu_count();
    out.procs = (uint32_t)process_count();
    return copy_to_user(uout, &out, sizeof(out));
}

static long sys_proclist(struct user_procinfo *uout, int max)
{
    if (max <= 0 || max > 256)
        return -EINVAL;
    if (!user_range_ok((uint64_t)uout, sizeof(*uout) * (uint64_t)max))
        return -EFAULT;
    struct user_procinfo *list = kzalloc(sizeof(*list) * (size_t)max);
    if (!list)
        return -ENOMEM;
    int n = process_list(list, max);
    long r = copy_to_user(uout, list, sizeof(*list) * (size_t)n);
    kfree(list);
    return r < 0 ? r : n;
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

static long sys_write(int fd, const void *ubuf, size_t len)
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

static long sys_openat(int dirfd, const char *upath, int flags)
{
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    struct vfs_node n;
    uint32_t mode = 0;
    bool exists = path_lookup(full, &n, &mode);
    if (exists && (flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;
    if (!exists && !(flags & O_CREAT))
        return -ENOENT;
    if (exists && (flags & O_DIRECTORY) && !n.is_dir)
        return -ENOTDIR;
    if (exists && n.is_dir && (flags & O_ACCMODE) != O_RDONLY)
        return -EISDIR;
    if (!exists && !vfs_writable())
        return -EROFS;
    struct file *f = file_open_path(full, flags);
    if (!f)
        return exists ? -EIO : -ENOENT;
    f->flags = (uint32_t)flags & ~(uint32_t)(O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC);
    f->path = kstrdup(full);
    int fd = process_fd_alloc(process_current(), f, 0, (flags & O_CLOEXEC) != 0);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static long sys_open(const char *upath, int flags)
{
    return sys_openat(AT_FDCWD, upath, flags);
}

static long sys_mkdirat(int dirfd, const char *upath)
{
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    struct vfs_node n;
    if (vfs_lookup(full, &n))
        return -EEXIST;
    if (!vfs_writable())
        return -EROFS;
    return vfs_create(full, true, NULL) ? 0 : -EIO;
}

static long sys_mkdir(const char *upath)
{
    return sys_mkdirat(AT_FDCWD, upath);
}

static long sys_unlinkat(int dirfd, const char *upath, bool is_dir)
{
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    struct vfs_node n;
    if (!vfs_lookup(full, &n))
        return -ENOENT;
    if (n.is_dir != is_dir)
        return is_dir ? -ENOTDIR : -EISDIR;
    if (!vfs_writable())
        return -EROFS;
    return vfs_unlink(full, is_dir) ? 0 : (is_dir ? -ENOTEMPTY : -EIO);
}

static long sys_unlink(const char *upath, bool is_dir)
{
    return sys_unlinkat(AT_FDCWD, upath, is_dir);
}

static long sys_renameat(int olddir, const char *ufrom, int newdir, const char *uto)
{
    char a[PROCESS_CWD_MAX], b[PROCESS_CWD_MAX];
    long r = resolve_at(olddir, ufrom, a, sizeof(a));
    if (r < 0)
        return r;
    r = resolve_at(newdir, uto, b, sizeof(b));
    if (r < 0)
        return r;
    if (!vfs_writable())
        return -EROFS;
    return vfs_rename(a, b) ? 0 : -EIO;
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
    if (!f->has_node)
        return -EINVAL;
    return vfs_truncate(&f->node, size) ? 0 : -EIO;
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
        if (fds[0] >= 0) process_fd_close(p, fds[0]); else file_unref(a);
        if (fds[1] >= 0) process_fd_close(p, fds[1]); else file_unref(b);
        return -EMFILE;
    }
    if (copy_to_user(ufds, fds, sizeof(fds)) < 0) {
        process_fd_close(p, fds[0]);
        process_fd_close(p, fds[1]);
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

static long sys_poll(struct user_pollfd *ufds, uint64_t nfds, int timeout)
{
    if (nfds > POLL_TABLE_MAX)
        return -EINVAL;
    struct user_pollfd fds[POLL_TABLE_MAX];
    if (copy_from_user(fds, ufds, nfds * sizeof(*fds)) < 0)
        return -EFAULT;

    struct poll_table *pt = kzalloc(sizeof(*pt));
    if (!pt)
        return -ENOMEM;

    uint64_t deadline = timeout > 0 ? time_uptime_ms() + (uint64_t)timeout : 0;
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
    if (ret >= 0 && copy_to_user(ufds, fds, nfds * sizeof(*fds)) < 0)
        return -EFAULT;
    return ret;
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

static void fill_stat(const struct vfs_node *n, uint32_t mode, struct user_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_size = n ? (int64_t)n->size : 0;
    if (!mode)
        mode = (n && n->is_dir) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_ino = n ? n->ino : 0;
    st->st_dev = 1;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
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
    fill_stat(f->has_node ? &f->node : NULL, f->mode, &st);
    if (!f->has_node && !f->mode)
        st.st_mode = S_IFIFO | 0600;
    return copy_to_user(ust, &st, sizeof(st));
}

static long sys_newfstatat(int dirfd, const char *upath, struct user_stat *ust, int flags)
{
    if (flags & AT_EMPTY_PATH) {
        char c;
        if (copy_from_user(&c, upath, 1) == 0 && c == 0)
            return sys_fstat(dirfd, ust);
    }
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    if (!user_range_ok((uint64_t)ust, sizeof(*ust)))
        return -EFAULT;
    struct vfs_node n;
    uint32_t mode;
    if (!path_lookup(full, &n, &mode))
        return -ENOENT;
    struct user_stat st;
    fill_stat(&n, mode, &st);
    return copy_to_user(ust, &st, sizeof(st));
}

static long sys_faccessat(int dirfd, const char *upath, int mode)
{
    (void)mode;
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    return path_lookup(full, NULL, NULL) ? 0 : -ENOENT;
}

static long sys_readlinkat(int dirfd, const char *upath, char *ubuf, uint64_t size)
{
    (void)ubuf;
    (void)size;
    char full[PROCESS_CWD_MAX];
    long r = resolve_at(dirfd, upath, full, sizeof(full));
    if (r < 0)
        return r;
    return path_lookup(full, NULL, NULL) ? -EINVAL : -ENOENT;
}

static uint8_t dirent_type(const struct vfs_node *n, bool in_dev)
{
    if (n->is_dir)
        return DT_DIR;
    if (!in_dev)
        return DT_REG;
    return n->ino >= 100 ? DT_BLK : DT_CHR;
}

static long sys_getdents64(int fd, void *ubuf, uint64_t count)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->has_node || !f->node.is_dir)
        return -ENOTDIR;
    if (!user_range_ok((uint64_t)ubuf, count))
        return -EFAULT;
    bool in_dev = f->path && devfs_owns(f->path);
    uint8_t rec[sizeof(struct linux_dirent64) + VFS_NAME_MAX + 8];
    uint64_t used = 0;
    for (;;) {
        struct vfs_node n;
        int r = file_readdir(f, f->offset, &n);
        if (r < 0)
            return used ? (long)used : r;
        if (r == 0)
            break;
        size_t namelen = strlen(n.name);
        uint16_t reclen = (uint16_t)ALIGN_UP(sizeof(struct linux_dirent64) + namelen + 1, 8);
        if (used + reclen > count) {
            if (!used)
                return -EINVAL;
            break;
        }
        memset(rec, 0, reclen);
        struct linux_dirent64 *d = (struct linux_dirent64 *)rec;
        d->d_ino = n.ino ? n.ino : f->offset + 1;
        d->d_off = (int64_t)(f->offset + 1);
        d->d_reclen = reclen;
        d->d_type = dirent_type(&n, in_dev);
        memcpy(d->d_name, n.name, namelen + 1);
        if (copy_to_user((uint8_t *)ubuf + used, rec, reclen) < 0)
            return used ? (long)used : -EFAULT;
        used += reclen;
        f->offset++;
    }
    return (long)used;
}

static long sys_lseek(int fd, int64_t off, int whence)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!f->has_node)
        return -ESPIPE;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->offset; break;
    case SEEK_END: base = (int64_t)f->node.size; break;
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
            if (!file_is_regular(f) || f->node.is_dir)
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
    if (!vma_add(p, base, base + len, vflags, f, off, f ? f->node.size : 0))
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
    if (flags & MAP_FIXED)
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
    size_t n = strlen(p->cwd) + 1;
    if (n > size)
        return -ERANGE;
    long r = copy_to_user(ubuf, p->cwd, n);
    return r < 0 ? r : (long)n;
}

static long sys_chdir(const char *upath)
{
    struct process *p = process_current();
    char full[PROCESS_CWD_MAX];
    long r = resolve_user(upath, full, sizeof(full));
    if (r < 0)
        return r;
    struct vfs_node n;
    if (!vfs_lookup(full, &n))
        return -ENOENT;
    if (!n.is_dir)
        return -ENOTDIR;
    strlcpy(p->cwd, full, sizeof(p->cwd));
    return 0;
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

static long sys_clock_gettime(int clk, struct user_timespec *uts)
{
    struct user_timespec ts;
    uint64_t us = time_now_us();
    switch (clk) {
    case CLOCK_REALTIME:
        ts.tv_sec = (int64_t)(rtc_boot_epoch() + us / 1000000);
        ts.tv_nsec = (int64_t)(us % 1000000) * 1000;
        break;
    case CLOCK_MONOTONIC:
        ts.tv_sec = (int64_t)(us / 1000000);
        ts.tv_nsec = (int64_t)(us % 1000000) * 1000;
        break;
    default:
        return -EINVAL;
    }
    return copy_to_user(uts, &ts, sizeof(ts));
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

static long sys_nanosleep(const struct user_timespec *ureq)
{
    struct user_timespec req;
    if (copy_from_user(&req, ureq, sizeof(req)) < 0)
        return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000)
        return -EINVAL;
    uint64_t ms = (uint64_t)req.tv_sec * 1000 + (uint64_t)req.tv_nsec / 1000000;
    thread_sleep_ms(ms ? ms : 1);
    return 0;
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

static long sys_wait4(int pid, int *ustatus, int options)
{
    if (ustatus && !user_range_ok((uint64_t)ustatus, sizeof(int)))
        return -EFAULT;
    int st = 0;
    int r = process_waitpid(pid, &st, options);
    if (r > 0 && ustatus && copy_to_user(ustatus, &st, sizeof(st)) < 0)
        return -EFAULT;
    return r;
}

static long sys_readdir(int fd, struct user_dirent *uout, uint64_t index)
{
    struct file *f = process_fd_pin(fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)uout, sizeof(*uout)))
        return -EFAULT;
    struct vfs_node n;
    int r = file_readdir(f, index, &n);
    if (r <= 0)
        return r;
    struct user_dirent d;
    memset(&d, 0, sizeof(d));
    d.size = n.size;
    d.is_dir = n.is_dir;
    strlcpy(d.name, n.name, sizeof(d.name));
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
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        struct user_iovec v;
        if (copy_from_user(&v, &uiov[i], sizeof(v)) < 0)
            return total ? total : -EFAULT;
        if (v.len == 0)
            continue;
        long n = write ? sys_write(fd, (const void *)v.base, v.len) : sys_read(fd, (void *)v.base, v.len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint64_t)n < v.len)
            break;
    }
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
    return -EINTR;
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
    case SYS_OPEN:        ret = sys_open((const char *)f->rdi, (int)f->rsi); break;
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
    case SYS_GETPPID:     ret = process_getppid(); break;
    case SYS_ARCH_PRCTL:  ret = sys_arch_prctl((int)f->rdi, f->rsi); break;
    case SYS_CLOCK_GETTIME: ret = sys_clock_gettime((int)f->rdi, (struct user_timespec *)f->rsi); break;
    case SYS_BRK:         ret = sys_brk(f->rdi); break;
    case SYS_PIPE:        ret = sys_pipe((int *)f->rdi); break;
    case SYS_SCHED_YIELD: thread_yield(); ret = 0; break;
    case SYS_DUP2:        ret = sys_dup2((int)f->rdi, (int)f->rsi); break;
    case SYS_NANOSLEEP:   ret = sys_nanosleep((const struct user_timespec *)f->rdi); break;
    case SYS_GETPID:      ret = process_current()->pid; break;
    case SYS_FORK:        ret = process_fork(f); break;
    case SYS_EXECVE:      ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)f->rdx, f); break;
    case SYS_EXIT:        process_exit_thread((int)f->rdi);
    case SYS_WAIT4:       ret = sys_wait4((int)f->rdi, (int *)f->rsi, (int)f->rdx); break;
    case SYS_READDIR:     ret = sys_readdir((int)f->rdi, (struct user_dirent *)f->rsi, f->rdx); break;
    case SYS_OPENPTY:     ret = sys_openpty((int *)f->rdi); break;
    case SYS_POLL:        ret = sys_poll((struct user_pollfd *)f->rdi, f->rsi, (int)f->rdx); break;
    case SYS_FCNTL:       ret = sys_fcntl((int)f->rdi, (int)f->rsi, f->rdx); break;
    case SYS_KILL:        ret = sys_kill((int)f->rdi, (int)f->rsi); break;
    case SYS_RT_SIGACTION: ret = sys_rt_sigaction((int)f->rdi, (const struct user_sigaction *)f->rsi, (struct user_sigaction *)f->rdx, f->r10); break;
    case SYS_RT_SIGPROCMASK: ret = sys_rt_sigprocmask((int)f->rdi, (const uint64_t *)f->rsi, (uint64_t *)f->rdx, f->r10); break;
    case SYS_LSTAT:       ret = sys_stat((const char *)f->rdi, (struct user_stat *)f->rsi); break;
    case SYS_ACCESS:      ret = sys_faccessat(AT_FDCWD, (const char *)f->rdi, (int)f->rsi); break;
    case SYS_READLINK:    ret = sys_readlinkat(AT_FDCWD, (const char *)f->rdi, (char *)f->rsi, f->rdx); break;
    case SYS_GETDENTS64:  ret = sys_getdents64((int)f->rdi, (void *)f->rsi, f->rdx); break;
    case SYS_OPENAT:      ret = sys_openat((int)f->rdi, (const char *)f->rsi, (int)f->rdx); break;
    case SYS_MKDIRAT:     ret = sys_mkdirat((int)f->rdi, (const char *)f->rsi); break;
    case SYS_NEWFSTATAT:  ret = sys_newfstatat((int)f->rdi, (const char *)f->rsi, (struct user_stat *)f->rdx, (int)f->r10); break;
    case SYS_UNLINKAT:    ret = sys_unlinkat((int)f->rdi, (const char *)f->rsi, (f->rdx & AT_REMOVEDIR) != 0); break;
    case SYS_RENAMEAT:    ret = sys_renameat((int)f->rdi, (const char *)f->rsi, (int)f->rdx, (const char *)f->r10); break;
    case SYS_READLINKAT:  ret = sys_readlinkat((int)f->rdi, (const char *)f->rsi, (char *)f->rdx, f->r10); break;
    case SYS_FACCESSAT:   ret = sys_faccessat((int)f->rdi, (const char *)f->rsi, (int)f->rdx); break;
    case SYS_DUP3:        ret = sys_dup3((int)f->rdi, (int)f->rsi, (int)f->rdx); break;
    case SYS_PIPE2:       ret = sys_pipe2((int *)f->rdi, (int)f->rsi); break;
    case SYS_READV:       ret = sys_readv((int)f->rdi, (const struct user_iovec *)f->rsi, (int)f->rdx); break;
    case SYS_WRITEV:      ret = sys_writev((int)f->rdi, (const struct user_iovec *)f->rsi, (int)f->rdx); break;
    case SYS_UNAME:       ret = sys_uname((struct user_utsname *)f->rdi); break;
    case SYS_GETUID:
    case SYS_GETGID:
    case SYS_GETEUID:
    case SYS_GETEGID:     ret = 0; break;
    case SYS_GETTID:      ret = thread_current()->utid; break;
    case SYS_SET_TID_ADDRESS: thread_current()->clear_child_tid = f->rdi; ret = thread_current()->utid; break;
    case SYS_CLONE:       ret = process_clone(f, f->rdi, f->rsi, f->rdx, f->r10, f->r8); break;
    case SYS_VFORK:       ret = process_fork(f); break;
    case SYS_PAUSE:       ret = sys_pause(); break;
    case SYS_EXIT_GROUP:  process_exit((int)f->rdi);
    case SYS_SCHED_GETAFFINITY: ret = sys_sched_getaffinity((int)f->rdi, f->rsi, (void *)f->rdx); break;
    case SYS_SIGALTSTACK: ret = sys_sigaltstack((const struct user_stack_t *)f->rdi, (struct user_stack_t *)f->rsi); break;
    case SYS_RT_SIGRETURN: ret = signal_sigreturn(f); full_restore = true; break;
    case SYS_ALARM:       ret = process_alarm(f->rdi); break;
    case SYS_SETPGID:     ret = process_setpgid((int)f->rdi, (int)f->rsi); break;
    case SYS_MKDIR:       ret = sys_mkdir((const char *)f->rdi); break;
    case SYS_RMDIR:       ret = sys_unlink((const char *)f->rdi, true); break;
    case SYS_UNLINK:      ret = sys_unlink((const char *)f->rdi, false); break;
    case SYS_RENAME:      ret = sys_rename((const char *)f->rdi, (const char *)f->rsi); break;
    case SYS_FTRUNCATE:   ret = sys_ftruncate((int)f->rdi, f->rsi); break;
    case SYS_FSYNC:
    case SYS_SYNC:        ret = vfs_sync() ? 0 : -EIO; break;
    case SYS_GETPGID:     ret = process_getpgid((int)f->rdi); break;
    case SYS_GETRLIMIT:   ret = sys_getrlimit((int)f->rdi, (struct user_rlimit *)f->rsi); break;
    case SYS_SETRLIMIT:   ret = sys_setrlimit((int)f->rdi, (const struct user_rlimit *)f->rsi); break;
    case SYS_PRLIMIT64:   ret = sys_prlimit64((int)f->rdi, (int)f->rsi, (const struct user_rlimit *)f->rdx, (struct user_rlimit *)f->r10); break;
    case SYS_SYSINFO:     ret = sys_sysinfo((struct user_sysinfo *)f->rdi); break;
    case SYS_PROCLIST:    ret = sys_proclist((struct user_procinfo *)f->rdi, (int)f->rsi); break;
    case SYS_SOCKET:      ret = sys_socket((int)f->rdi, (int)f->rsi, (int)f->rdx); break;
    case SYS_BIND:        ret = sys_bind((int)f->rdi, (const struct sockaddr_in *)f->rsi, (uint32_t)f->rdx); break;
    case SYS_CONNECT:     ret = sys_connect((int)f->rdi, (const struct sockaddr_in *)f->rsi, (uint32_t)f->rdx); break;
    case SYS_SENDTO:      ret = sys_sendto((int)f->rdi, (const void *)f->rsi, f->rdx, (int)f->r10, (const struct sockaddr_in *)f->r8, (uint32_t)f->r9); break;
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
