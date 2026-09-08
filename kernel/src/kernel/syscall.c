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
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel/smp.h>

extern void syscall_entry(void);

#define EFER_SCE (1ull << 0)

static bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return ptr < USER_ADDR_MAX && len <= USER_ADDR_MAX - ptr;
}

static bool user_str_ok(const char *s, size_t max)
{
    if (!user_range_ok((uint64_t)s, 1))
        return false;
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok((uint64_t)(s + i), 1))
            return false;
        if (s[i] == '\0')
            return true;
    }
    return false;
}

static bool user_ptrs_ok(char *const *v)
{
    if (!v)
        return true;
    for (int i = 0; i < 256; i++) {
        if (!user_range_ok((uint64_t)&v[i], 8))
            return false;
        if (!v[i])
            return true;
        if (!user_str_ok(v[i], 4096))
            return false;
    }
    return false;
}

static long sys_sysinfo(struct user_sysinfo *out)
{
    if (!user_range_ok((uint64_t)out, sizeof(*out)))
        return -EFAULT;
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t faults, copies;
    vmm_cow_stats(&faults, &copies);
    out->total_pages = st.total_pages;
    out->used_pages = st.used_pages;
    out->free_pages = st.free_pages;
    out->shared_pages = pmm_shared_pages();
    out->cow_faults = faults;
    out->uptime_ms = time_uptime_ms();
    struct heap_stats hs;
    heap_get_stats(&hs);
    out->heap_used = hs.slab_bytes_used + hs.large_bytes_used;
    out->cpus = smp_cpu_count();
    out->procs = 0;
    return 0;
}

static long sys_proclist(struct user_procinfo *out, int max)
{
    if (max <= 0 || max > 64)
        return -EINVAL;
    if (!user_range_ok((uint64_t)out, sizeof(*out) * (uint64_t)max))
        return -EFAULT;
    return process_list(out, max);
}

static long sys_read(int fd, void *buf, size_t len)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)buf, len))
        return -EFAULT;
    return file_read(f, buf, len);
}

static long sys_write(int fd, const void *buf, size_t len)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)buf, len))
        return -EFAULT;
    return file_write(f, buf, len);
}

static long sys_open(const char *path, int flags)
{
    if (!user_str_ok(path, 512))
        return -EFAULT;
    char full[PROCESS_CWD_MAX];
    if (!process_resolve_path(process_current(), path, full, sizeof(full)))
        return -ENOENT;
    struct file *f = file_open_path(full, flags);
    if (!f)
        return -ENOENT;
    int fd = process_fd_install(process_current(), f);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static long resolved(const char *path, char *out, size_t size)
{
    if (!user_str_ok(path, 512))
        return -EFAULT;
    if (!process_resolve_path(process_current(), path, out, size))
        return -ENOENT;
    return 0;
}

static long sys_mkdir(const char *path)
{
    char full[PROCESS_CWD_MAX];
    long r = resolved(path, full, sizeof(full));
    if (r < 0)
        return r;
    struct vfs_node n;
    if (vfs_lookup(full, &n))
        return -EEXIST;
    if (!vfs_writable())
        return -EROFS;
    return vfs_create(full, true, NULL) ? 0 : -EIO;
}

static long sys_unlink(const char *path, bool is_dir)
{
    char full[PROCESS_CWD_MAX];
    long r = resolved(path, full, sizeof(full));
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

static long sys_rename(const char *from, const char *to)
{
    char a[PROCESS_CWD_MAX], b[PROCESS_CWD_MAX];
    long r = resolved(from, a, sizeof(a));
    if (r < 0)
        return r;
    r = resolved(to, b, sizeof(b));
    if (r < 0)
        return r;
    if (!vfs_writable())
        return -EROFS;
    return vfs_rename(a, b) ? 0 : -EIO;
}

static long sys_ftruncate(int fd, uint64_t size)
{
    struct file *f = process_fd_get(process_current(), fd);
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

static long sys_pipe(int *fds)
{
    if (!user_range_ok((uint64_t)fds, 8))
        return -EFAULT;
    struct file *r, *w;
    if (!file_make_pipe(&r, &w))
        return -ENOMEM;
    struct process *p = process_current();
    int rfd = process_fd_install(p, r);
    int wfd = process_fd_install(p, w);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) process_fd_close(p, rfd); else file_unref(r);
        if (wfd >= 0) process_fd_close(p, wfd); else file_unref(w);
        return -EMFILE;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

static long sys_openpty(int *fds)
{
    if (!user_range_ok((uint64_t)fds, 2 * sizeof(int)))
        return -EFAULT;
    struct file *master, *slave;
    int r = pty_open_pair(&master, &slave);
    if (r < 0)
        return r;
    struct process *p = process_current();
    int mfd = process_fd_install(p, master);
    int sfd = process_fd_install(p, slave);
    if (mfd < 0 || sfd < 0) {
        if (mfd >= 0) process_fd_close(p, mfd); else file_unref(master);
        if (sfd >= 0) process_fd_close(p, sfd); else file_unref(slave);
        return -EMFILE;
    }
    fds[0] = mfd;
    fds[1] = sfd;
    return 0;
}

static long sys_poll(struct user_pollfd *fds, uint64_t nfds, int timeout)
{
    if (nfds > POLL_TABLE_MAX)
        return -EINVAL;
    if (nfds && !user_range_ok((uint64_t)fds, nfds * sizeof(*fds)))
        return -EFAULT;

    struct process *p = process_current();
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
            struct file *f = process_fd_get(p, fds[i].fd);
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
    }
    kfree(pt);
    return ret;
}

static long sys_fcntl(int fd, int cmd, uint64_t arg)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case F_GETFL:
        return (long)f->flags;
    case F_SETFL:
        f->flags = (uint32_t)arg & O_NONBLOCK;
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

static long sys_rt_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old)
{
    if (act && !user_range_ok((uint64_t)act, sizeof(*act)))
        return -EFAULT;
    if (old && !user_range_ok((uint64_t)old, sizeof(*old)))
        return -EFAULT;
    return signal_sigaction(sig, act, old);
}

static long sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *old)
{
    if (set && !user_range_ok((uint64_t)set, sizeof(*set)))
        return -EFAULT;
    if (old && !user_range_ok((uint64_t)old, sizeof(*old)))
        return -EFAULT;
    return signal_procmask(how, set, old);
}

static void fill_stat(const struct vfs_node *n, uint32_t mode, struct user_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_size = n ? n->size : 0;
    if (!mode)
        mode = (n && n->is_dir) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_ino = n ? n->ino : 0;
    st->st_blksize = 4096;
}

static long sys_stat(const char *path, struct user_stat *st)
{
    if (!user_str_ok(path, 512) || !user_range_ok((uint64_t)st, sizeof(*st)))
        return -EFAULT;
    char full[PROCESS_CWD_MAX];
    if (!process_resolve_path(process_current(), path, full, sizeof(full)))
        return -ENOENT;
    struct vfs_node n;
    if (!vfs_lookup(full, &n))
        return -ENOENT;
    fill_stat(&n, devfs_mode(full), st);
    return 0;
}

static long sys_fstat(int fd, struct user_stat *st)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)st, sizeof(*st)))
        return -EFAULT;
    fill_stat(f->has_node ? &f->node : NULL, f->mode, st);
    if (!f->has_node && !f->mode)
        st->st_mode = S_IFIFO | 0600;
    return 0;
}

static long sys_lseek(int fd, int64_t off, int whence)
{
    struct file *f = process_fd_get(process_current(), fd);
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

static long sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    (void)addr;
    struct process *p = process_current();
    if (len == 0)
        return -EINVAL;
    if (fd >= 0) {
        struct file *f = process_fd_get(p, fd);
        if (!f)
            return -EBADF;
        return (long)file_mmap(f, len, prot, off);
    }
    if (fd != -1 || !(flags & MAP_ANONYMOUS))
        return -EINVAL;
    uint64_t pages = PAGE_ALIGN_UP(len) / PAGE_SIZE;
    uint64_t base = p->mmap_next;
    if (base + pages * PAGE_SIZE > USER_STACK_TOP - USER_STACK_SIZE - MiB(16))
        return -ENOMEM;
    uint64_t pf = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE)
        pf |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC))
        pf |= PTE_NX;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return -ENOMEM;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        vmm_map_page_in(p->pml4_phys, base + i * PAGE_SIZE, phys, pf);
        p->pages++;
    }
    p->mmap_next = base + pages * PAGE_SIZE;
    return (long)base;
}

static long sys_munmap(uint64_t addr, uint64_t len)
{
    struct process *p = process_current();
    if (!IS_ALIGNED(addr, PAGE_SIZE) || addr < USER_MMAP_BASE || addr + len > p->mmap_next)
        return -EINVAL;
    uint64_t pages = PAGE_ALIGN_UP(len) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = addr + i * PAGE_SIZE;
        uint64_t pte = vmm_get_pte_in(p->pml4_phys, v);
        if (!(pte & PTE_PRESENT))
            continue;
        vmm_unmap_page_in(p->pml4_phys, v);
        if (!(pte & PTE_NOFREE)) {
            pmm_free_page(pte & PTE_ADDR_MASK);
            p->pages--;
        }
    }
    return 0;
}

static long sys_ioctl(int fd, uint64_t request, uint64_t arg)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    return file_ioctl(f, request, arg);
}

static long sys_dup(int oldfd)
{
    struct process *p = process_current();
    struct file *f = process_fd_get(p, oldfd);
    if (!f)
        return -EBADF;
    file_ref(f);
    int fd = process_fd_install(p, f);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static long sys_getcwd(char *buf, size_t size)
{
    struct process *p = process_current();
    if (!user_range_ok((uint64_t)buf, size))
        return -EFAULT;
    size_t n = strlen(p->cwd) + 1;
    if (n > size)
        return -ERANGE;
    memcpy(buf, p->cwd, n);
    return (long)n;
}

static long sys_chdir(const char *path)
{
    if (!user_str_ok(path, 512))
        return -EFAULT;
    struct process *p = process_current();
    char full[PROCESS_CWD_MAX];
    if (!process_resolve_path(p, path, full, sizeof(full)))
        return -ENOENT;
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
    case ARCH_GET_FS:
        if (!user_range_ok(addr, 8))
            return -EFAULT;
        *(uint64_t *)addr = thread_current()->fs_base;
        return 0;
    default:
        return -EINVAL;
    }
}

static long sys_clock_gettime(int clk, struct user_timespec *ts)
{
    if (!user_range_ok((uint64_t)ts, sizeof(*ts)))
        return -EFAULT;
    uint64_t us = time_now_us();
    switch (clk) {
    case CLOCK_REALTIME:
        ts->tv_sec = (int64_t)(rtc_boot_epoch() + us / 1000000);
        ts->tv_nsec = (int64_t)(us % 1000000) * 1000;
        return 0;
    case CLOCK_MONOTONIC:
        ts->tv_sec = (int64_t)(us / 1000000);
        ts->tv_nsec = (int64_t)(us % 1000000) * 1000;
        return 0;
    default:
        return -EINVAL;
    }
}

static long sys_dup2(int oldfd, int newfd)
{
    struct process *p = process_current();
    struct file *f = process_fd_get(p, oldfd);
    if (!f || newfd < 0 || newfd >= PROCESS_MAX_FDS)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;
    if (p->fds[newfd])
        file_unref(p->fds[newfd]);
    file_ref(f);
    p->fds[newfd] = f;
    return newfd;
}

static long sys_brk(uint64_t addr)
{
    struct process *p = process_current();
    if (addr == 0)
        return (long)p->brk;
    if (addr < p->brk_start || addr > p->brk_start + USER_HEAP_MAX)
        return -ENOMEM;
    uint64_t old_end = PAGE_ALIGN_UP(p->brk);
    uint64_t new_end = PAGE_ALIGN_UP(addr);
    for (uint64_t v = old_end; v < new_end; v += PAGE_SIZE) {
        if (vmm_virt_to_phys_in(p->pml4_phys, v))
            continue;
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return -ENOMEM;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        vmm_map_page_in(p->pml4_phys, v, phys, PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX);
        p->pages++;
    }
    for (uint64_t v = new_end; v < old_end; v += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys_in(p->pml4_phys, v);
        if (phys) {
            vmm_unmap_page_in(p->pml4_phys, v);
            pmm_free_page(PAGE_ALIGN_DOWN(phys));
            p->pages--;
        }
    }
    p->brk = addr;
    return (long)addr;
}

struct timespec_user {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static long sys_nanosleep(const struct timespec_user *req)
{
    if (!user_range_ok((uint64_t)req, sizeof(*req)))
        return -EFAULT;
    uint64_t ms = (uint64_t)req->tv_sec * 1000 + (uint64_t)req->tv_nsec / 1000000;
    thread_sleep_ms(ms ? ms : 1);
    return 0;
}

static long sys_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *f)
{
    if (!user_str_ok(path, 512) || !user_ptrs_ok(argv) || !user_ptrs_ok(envp))
        return -EFAULT;
    return process_execve(path, argv, envp, f);
}

static long sys_wait4(int pid, int *status, int options)
{
    if (status && !user_range_ok((uint64_t)status, 4))
        return -EFAULT;
    int st = 0;
    int r = process_waitpid(pid, &st, options);
    if (r > 0 && status)
        *status = st;
    return r;
}

static long sys_readdir(int fd, struct user_dirent *out, uint64_t index)
{
    struct file *f = process_fd_get(process_current(), fd);
    if (!f)
        return -EBADF;
    if (!user_range_ok((uint64_t)out, sizeof(*out)))
        return -EFAULT;
    struct vfs_node n;
    int r = file_readdir(f, index, &n);
    if (r <= 0)
        return r;
    out->size = n.size;
    out->is_dir = n.is_dir;
    strlcpy(out->name, n.name, sizeof(out->name));
    return 1;
}

void syscall_dispatch(struct interrupt_frame *f)
{
    sti();
    long ret;
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
    case SYS_IOCTL:       ret = sys_ioctl((int)f->rdi, f->rsi, f->rdx); break;
    case SYS_DUP:         ret = sys_dup((int)f->rdi); break;
    case SYS_GETCWD:      ret = sys_getcwd((char *)f->rdi, (size_t)f->rsi); break;
    case SYS_CHDIR:       ret = sys_chdir((const char *)f->rdi); break;
    case SYS_GETPPID:     ret = process_current()->parent ? (long)process_current()->parent->pid : 0; break;
    case SYS_ARCH_PRCTL:  ret = sys_arch_prctl((int)f->rdi, f->rsi); break;
    case SYS_CLOCK_GETTIME: ret = sys_clock_gettime((int)f->rdi, (struct user_timespec *)f->rsi); break;
    case SYS_BRK:         ret = sys_brk(f->rdi); break;
    case SYS_PIPE:        ret = sys_pipe((int *)f->rdi); break;
    case SYS_SCHED_YIELD: thread_yield(); ret = 0; break;
    case SYS_DUP2:        ret = sys_dup2((int)f->rdi, (int)f->rsi); break;
    case SYS_NANOSLEEP:   ret = sys_nanosleep((const struct timespec_user *)f->rdi); break;
    case SYS_GETPID:      ret = process_current()->pid; break;
    case SYS_FORK:        ret = process_fork(f); break;
    case SYS_EXECVE:      ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)f->rdx, f); break;
    case SYS_EXIT:        process_exit((int)f->rdi);
    case SYS_WAIT4:       ret = sys_wait4((int)f->rdi, (int *)f->rsi, (int)f->rdx); break;
    case SYS_READDIR:     ret = sys_readdir((int)f->rdi, (struct user_dirent *)f->rsi, f->rdx); break;
    case SYS_OPENPTY:     ret = sys_openpty((int *)f->rdi); break;
    case SYS_POLL:        ret = sys_poll((struct user_pollfd *)f->rdi, f->rsi, (int)f->rdx); break;
    case SYS_FCNTL:       ret = sys_fcntl((int)f->rdi, (int)f->rsi, f->rdx); break;
    case SYS_KILL:        ret = sys_kill((int)f->rdi, (int)f->rsi); break;
    case SYS_RT_SIGACTION: ret = sys_rt_sigaction((int)f->rdi, (const struct user_sigaction *)f->rsi, (struct user_sigaction *)f->rdx); break;
    case SYS_RT_SIGPROCMASK: ret = sys_rt_sigprocmask((int)f->rdi, (const uint64_t *)f->rsi, (uint64_t *)f->rdx); break;
    case SYS_RT_SIGRETURN: ret = signal_sigreturn(f); break;
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
    signal_check(f);
}

void syscall_init_cpu(void)
{
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32) | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_IA32_FMASK, RFLAGS_IF | (1 << 10) | (1 << 8));
    wrmsr(MSR_IA32_EFER, rdmsr(MSR_IA32_EFER) | EFER_SCE);
}

void syscall_init(void)
{
    syscall_init_cpu();
    log_info("syscall", "syscall/sysret ativos, entrada em %p", (void *)syscall_entry);
}
