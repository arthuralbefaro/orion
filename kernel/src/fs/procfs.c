#include <fs/procfs.h>
#include <fs/vfs.h>
#include <fs/block.h>
#include <kernel/file.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/version.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/vma.h>
#include <mm/memory.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <stdarg.h>

enum pkind {
    P_ROOT = 1,
    P_MEMINFO,
    P_CPUINFO,
    P_UPTIME,
    P_MOUNTS,
    P_LOADAVG,
    P_VERSION,
    P_DISKSTATS,
    P_SYSSTAT,
    P_PIDDIR = 16,
    P_STATUS,
    P_CMDLINE,
    P_MAPS,
    P_STAT,
    P_FDDIR,
    P_FD,
};

struct pent {
    const char *name;
    enum pkind kind;
};

static const struct pent root_ents[] = {
    { "meminfo", P_MEMINFO }, { "cpuinfo", P_CPUINFO }, { "uptime", P_UPTIME },
    { "mounts", P_MOUNTS },   { "loadavg", P_LOADAVG }, { "version", P_VERSION },
    { "diskstats", P_DISKSTATS }, { "stat", P_SYSSTAT },
};

static const struct pent pid_ents[] = {
    { "status", P_STATUS }, { "cmdline", P_CMDLINE }, { "maps", P_MAPS }, { "stat", P_STAT }, { "fd", P_FDDIR },
};

#define PROC_PID_COOKIE 1000
#define PROC_MAX_PIDS   1024

static uint64_t mkkey(uint32_t pid, enum pkind kind, uint32_t fd)
{
    return ((uint64_t)pid << 32) | ((uint64_t)kind << 16) | fd;
}

static uint32_t key_pid(const struct vnode *v)
{
    return (uint32_t)(v->key >> 32);
}

static enum pkind key_kind(const struct vnode *v)
{
    return (enum pkind)((v->key >> 16) & 0xFFFF);
}

static uint32_t key_fd(const struct vnode *v)
{
    return (uint32_t)(v->key & 0xFFFF);
}

static bool kind_is_dir(enum pkind k)
{
    return k == P_ROOT || k == P_PIDDIR || k == P_FDDIR;
}

static uint32_t kind_mode(enum pkind k)
{
    return kind_is_dir(k) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
}

static int proc_vnode(struct mount *m, uint32_t pid, enum pkind kind, uint32_t fd, struct vnode **out)
{
    uint64_t key = mkkey(pid, kind, fd);
    struct vnode *v = vcache_find(m, key);
    if (!v) {
        v = vnode_new(m, key, kind_mode(kind));
        if (!v)
            return -ENOMEM;
        v = vcache_insert(v);
    }
    *out = v;
    return 0;
}

static bool parse_uint(const char *s, uint32_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFull)
            return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool process_alive(uint32_t pid)
{
    struct process *p = process_get(pid);
    process_put(p);
    return p != NULL;
}

static bool fd_open(struct process *p, uint32_t fd)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    bool ok = (int)fd < p->fd_cap && p->fds[fd] != NULL;
    spin_unlock_irqrestore(&p->tlock, flags);
    return ok;
}

static int proc_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct mount *m = dir->mnt;
    uint32_t pid = key_pid(dir);
    enum pkind kind = key_kind(dir);
    bool dotdot = name[0] == '.' && name[1] == '.' && !name[2];
    if (name[0] == '.' && !name[1]) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    if (kind == P_ROOT) {
        if (dotdot) {
            vnode_get(dir);
            *out = dir;
            return 0;
        }
        if (strcmp(name, "self") == 0) {
            struct thread *t = thread_current();
            if (!t || !t->process)
                return -ENOENT;
            return proc_vnode(m, t->process->pid, P_PIDDIR, 0, out);
        }
        uint32_t n;
        if (parse_uint(name, &n))
            return process_alive(n) ? proc_vnode(m, n, P_PIDDIR, 0, out) : -ENOENT;
        for (size_t i = 0; i < ARRAY_SIZE(root_ents); i++)
            if (strcmp(root_ents[i].name, name) == 0)
                return proc_vnode(m, 0, root_ents[i].kind, 0, out);
        return -ENOENT;
    }
    if (kind == P_PIDDIR) {
        if (dotdot)
            return proc_vnode(m, 0, P_ROOT, 0, out);
        if (!process_alive(pid))
            return -ENOENT;
        for (size_t i = 0; i < ARRAY_SIZE(pid_ents); i++)
            if (strcmp(pid_ents[i].name, name) == 0)
                return proc_vnode(m, pid, pid_ents[i].kind, 0, out);
        return -ENOENT;
    }
    if (kind == P_FDDIR) {
        if (dotdot)
            return proc_vnode(m, pid, P_PIDDIR, 0, out);
        uint32_t fd;
        if (!parse_uint(name, &fd) || fd >= 0xFFFF)
            return -ENOENT;
        struct process *p = process_get(pid);
        bool ok = p && fd_open(p, fd);
        process_put(p);
        return ok ? proc_vnode(m, pid, P_FD, fd, out) : -ENOENT;
    }
    return -ENOTDIR;
}

static bool emit(vfs_dir_cb cb, void *ctx, uint64_t *pos, uint64_t cookie, const char *name, uint64_t ino,
                 uint32_t mode)
{
    struct vfs_dirent e;
    strlcpy(e.name, name, sizeof(e.name));
    e.ino = ino;
    e.mode = mode;
    e.size = 0;
    e.next = cookie + 1;
    if (!cb(&e, ctx))
        return false;
    *pos = cookie + 1;
    return true;
}

static void sort_u32(uint32_t *a, int n)
{
    for (int i = 1; i < n; i++) {
        uint32_t x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > x) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static int proc_readdir(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    uint32_t pid = key_pid(dir);
    enum pkind kind = key_kind(dir);
    char name[16];
    if (kind == P_ROOT) {
        uint64_t nfixed = ARRAY_SIZE(root_ents);
        for (uint64_t i = *pos; i < nfixed; i++)
            if (!emit(cb, ctx, pos, i, root_ents[i].name, mkkey(0, root_ents[i].kind, 0),
                      kind_mode(root_ents[i].kind)))
                return 0;
        struct thread *t = thread_current();
        if (*pos <= nfixed && t && t->process &&
            !emit(cb, ctx, pos, nfixed, "self", mkkey(t->process->pid, P_PIDDIR, 0), kind_mode(P_PIDDIR)))
            return 0;
        if (*pos <= nfixed)
            *pos = PROC_PID_COOKIE;
        uint32_t *pids = kmalloc(sizeof(uint32_t) * PROC_MAX_PIDS);
        if (!pids)
            return -ENOMEM;
        int n = process_pids(pids, PROC_MAX_PIDS);
        sort_u32(pids, n);
        for (int i = 0; i < n; i++) {
            uint64_t cookie = PROC_PID_COOKIE + pids[i];
            if (cookie < *pos)
                continue;
            ksnprintf(name, sizeof(name), "%u", pids[i]);
            if (!emit(cb, ctx, pos, cookie, name, mkkey(pids[i], P_PIDDIR, 0), kind_mode(P_PIDDIR)))
                break;
        }
        kfree(pids);
        return 0;
    }
    if (kind == P_PIDDIR) {
        if (!process_alive(pid))
            return 0;
        for (uint64_t i = *pos; i < ARRAY_SIZE(pid_ents); i++)
            if (!emit(cb, ctx, pos, i, pid_ents[i].name, mkkey(pid, pid_ents[i].kind, 0),
                      kind_mode(pid_ents[i].kind)))
                return 0;
        return 0;
    }
    if (kind == P_FDDIR) {
        struct process *p = process_get(pid);
        if (!p)
            return 0;
        uint8_t *open = kzalloc(PROCESS_FD_MAX);
        int cap = 0;
        if (open) {
            uint64_t flags = spin_lock_irqsave(&p->tlock);
            cap = p->fd_cap < PROCESS_FD_MAX ? p->fd_cap : PROCESS_FD_MAX;
            for (int i = 0; i < cap; i++)
                open[i] = p->fds[i] != NULL;
            spin_unlock_irqrestore(&p->tlock, flags);
        }
        process_put(p);
        for (uint64_t i = *pos; open && i < (uint64_t)cap; i++) {
            if (!open[i])
                continue;
            ksnprintf(name, sizeof(name), "%lu", i);
            if (!emit(cb, ctx, pos, i, name, mkkey(pid, P_FD, (uint32_t)i), kind_mode(P_FD)))
                break;
        }
        kfree(open);
        return open ? 0 : -ENOMEM;
    }
    return -ENOTDIR;
}

struct pbuf {
    char *buf;
    size_t len;
    size_t cap;
    bool failed;
};

static void pb_putc(char c, void *ctx)
{
    struct pbuf *b = ctx;
    if (b->failed)
        return;
    if (b->len + 1 >= b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        char *n = krealloc(b->buf, cap);
        if (!n) {
            b->failed = true;
            return;
        }
        b->buf = n;
        b->cap = cap;
    }
    b->buf[b->len++] = c;
}

static void pb_printf(struct pbuf *b, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void pb_printf(struct pbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvformat(pb_putc, b, fmt, ap);
    va_end(ap);
}

static void pb_bytes(struct pbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        pb_putc(s[i], b);
}

static void gen_meminfo(struct pbuf *b)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    struct heap_stats hs;
    heap_get_stats(&hs);
    uint64_t faults, copies;
    vmm_cow_stats(&faults, &copies);
    uint64_t kb = PAGE_SIZE / 1024;
    pb_printf(b, "MemTotal:       %8lu kB\n", st.total_pages * kb);
    pb_printf(b, "MemFree:        %8lu kB\n", st.free_pages * kb);
    pb_printf(b, "MemAvailable:   %8lu kB\n", (st.free_pages + vfs_cache_reclaimable()) * kb);
    pb_printf(b, "Buffers:        %8lu kB\n", 0ul);
    pb_printf(b, "Cached:         %8lu kB\n", pcache_total_pages() * kb);
    pb_printf(b, "SwapTotal:      %8lu kB\n", 0ul);
    pb_printf(b, "SwapFree:       %8lu kB\n", 0ul);
    pb_printf(b, "KernelHeap:     %8lu kB\n", (hs.slab_bytes_used + hs.large_bytes_used) / 1024);
    pb_printf(b, "CowShared:      %8lu kB\n", pmm_shared_pages() * kb);
    pb_printf(b, "CowFaults:      %8lu\n", faults);
    pb_printf(b, "CowCopies:      %8lu\n", copies);
}

static void gen_cpuinfo(struct pbuf *b)
{
    char brand[49];
    cpu_get_brand(brand);
    struct cpuid_result v = cpuid(0, 0);
    char vendor[13];
    memcpy(vendor, &v.ebx, 4);
    memcpy(vendor + 4, &v.edx, 4);
    memcpy(vendor + 8, &v.ecx, 4);
    vendor[12] = 0;
    uint64_t khz = time_tsc_hz() / 1000;
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        pb_printf(b, "processor\t: %u\n", i);
        pb_printf(b, "vendor_id\t: %s\n", vendor);
        pb_printf(b, "model name\t: %s\n", brand);
        pb_printf(b, "cpu MHz\t\t: %lu.%03lu\n", khz / 1000, khz % 1000);
        pb_printf(b, "\n");
    }
}

static void gen_uptime(struct pbuf *b)
{
    uint64_t ms = time_uptime_ms();
    pb_printf(b, "%lu.%02lu 0.00\n", ms / 1000, (ms % 1000) / 10);
}

static void gen_loadavg(struct pbuf *b)
{
    uint32_t *pids = kmalloc(sizeof(uint32_t) * PROC_MAX_PIDS);
    int n = pids ? process_pids(pids, PROC_MAX_PIDS) : 0;
    uint32_t last = 0;
    for (int i = 0; i < n; i++)
        if (pids[i] > last)
            last = pids[i];
    kfree(pids);
    pb_printf(b, "0.00 0.00 0.00 1/%d %u\n", n, last);
}

static void gen_version(struct pbuf *b)
{
    pb_printf(b, "%s version %s (%s)\n", KERNEL_NAME, KERNEL_VERSION, KERNEL_ARCH);
}

static void gen_diskstats(struct pbuf *b)
{
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_at(i);
        pb_printf(b, "%4u %7u %s %lu 0 %lu 0 %lu 0 %lu 0 0 0 0\n", 8u, i, d->name, d->rd_ios, d->rd_sectors,
                  d->wr_ios, d->wr_sectors);
    }
}

static void stat_line(struct pbuf *b, const char *name, uint64_t user, uint64_t sys, uint64_t idle)
{
    uint64_t div = TIME_TICK_HZ / 100;
    pb_printf(b, "%s %lu 0 %lu %lu 0 0 0 0 0 0\n", name, user / div, sys / div, idle / div);
}

static void gen_sysstat(struct pbuf *b)
{
    uint64_t user = 0, sys = 0, idle = 0, ctxt = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = percpu_by_index(i);
        if (!c || !c->online)
            continue;
        user += c->user_ticks;
        sys += c->sys_ticks;
        idle += c->idle_ticks;
        ctxt += c->context_switches;
    }
    stat_line(b, "cpu ", user, sys, idle);
    char name[16];
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        struct percpu *c = percpu_by_index(i);
        if (!c || !c->online)
            continue;
        ksnprintf(name, sizeof(name), "cpu%u", c->cpu_id);
        stat_line(b, name, c->user_ticks, c->sys_ticks, c->idle_ticks);
    }
    pb_printf(b, "ctxt %lu\n", ctxt);
    pb_printf(b, "btime 0\n");
}

static void mount_line(const struct mount *m, void *ctx)
{
    pb_printf(ctx, "%s %s %s %s 0 0\n", m->source, m->point, m->type->name,
              (m->flags & MNT_RDONLY) ? "ro" : "rw");
}

static void gen_mounts(struct pbuf *b)
{
    vfs_mounts_foreach(mount_line, b);
}

static uint64_t vm_bytes(struct process *p)
{
    uint64_t total = 0;
    mutex_lock(&p->mm_lock);
    for (struct vma *v = p->vmas; v; v = v->next)
        total += v->end - v->start;
    mutex_unlock(&p->mm_lock);
    return total;
}

static char state_char(struct process *p)
{
    return p->state == PROC_ZOMBIE ? 'Z' : 'R';
}

static void gen_status(struct pbuf *b, struct process *p)
{
    pb_printf(b, "Name:\t%s\n", p->name);
    pb_printf(b, "State:\t%c (%s)\n", state_char(p), p->state == PROC_ZOMBIE ? "zombie" : "running");
    pb_printf(b, "Tgid:\t%u\n", p->pid);
    pb_printf(b, "Pid:\t%u\n", p->pid);
    pb_printf(b, "PPid:\t%u\n", process_ppid(p));
    pb_printf(b, "Pgid:\t%u\n", p->pgid);
    pb_printf(b, "Threads:\t%u\n", p->live_threads);
    pb_printf(b, "VmSize:\t%lu kB\n", vm_bytes(p) / 1024);
    pb_printf(b, "VmRSS:\t%lu kB\n", p->pages * (PAGE_SIZE / 1024));
}

static void gen_stat(struct pbuf *b, struct process *p)
{
    struct cpu_times self, kids;
    process_cpu_times(p, &self, &kids);
    pb_printf(b, "%u (%s) %c %u %u 0 0 -1 0 0 0 0 0 %lu %lu %lu %lu 20 0 %u 0 0 %lu %lu\n", p->pid, p->name,
              state_char(p), process_ppid(p), p->pgid, self.utime / 10000, self.stime / 10000,
              kids.utime / 10000, kids.stime / 10000, p->live_threads, vm_bytes(p), p->pages);
}

static void gen_cmdline(struct pbuf *b, struct process *p)
{
    char *tmp = kmalloc(PROCESS_CMDLINE_MAX);
    if (!tmp) {
        b->failed = true;
        return;
    }
    uint32_t n = process_cmdline(p, tmp, PROCESS_CMDLINE_MAX);
    pb_bytes(b, tmp, n);
    kfree(tmp);
}

static void gen_maps(struct pbuf *b, struct process *p)
{
    mutex_lock(&p->mm_lock);
    for (struct vma *v = p->vmas; v; v = v->next) {
        const char *name = "";
        uint64_t ino = 0;
        if (v->file) {
            name = v->file->path ? v->file->path : "";
            ino = v->file->vn ? v->file->vn->ino : 0;
        } else if (v->flags & VMA_HEAP) {
            name = "[heap]";
        } else if (v->flags & VMA_STACK) {
            name = "[stack]";
        } else if (v->flags & VMA_IMAGE) {
            name = p->exe;
        }
        pb_printf(b, "%08lx-%08lx %c%c%c%c %08lx 00:00 %-8lu %s\n", v->start, v->end,
                  (v->flags & VMA_READ) ? 'r' : '-', (v->flags & VMA_WRITE) ? 'w' : '-',
                  (v->flags & VMA_EXEC) ? 'x' : '-', (v->flags & VMA_SHARED) ? 's' : 'p', v->offset, ino,
                  name);
    }
    mutex_unlock(&p->mm_lock);
}

static bool gen_fd(struct pbuf *b, struct process *p, uint32_t fd)
{
    char path[PROCESS_CWD_MAX];
    path[0] = 0;
    uint32_t mode = 0;
    bool tty = false, found = false;
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if ((int)fd < p->fd_cap && p->fds[fd]) {
        struct file *f = p->fds[fd];
        found = true;
        if (f->path)
            strlcpy(path, f->path, sizeof(path));
        mode = f->mode;
        tty = file_is_tty(f);
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    if (!found)
        return false;
    if (path[0])
        pb_printf(b, "%s\n", path);
    else if (tty)
        pb_printf(b, "/dev/tty\n");
    else if ((mode & S_IFMT) == S_IFIFO)
        pb_printf(b, "pipe:\n");
    else if ((mode & S_IFMT) == S_IFCHR)
        pb_printf(b, "char:\n");
    else
        pb_printf(b, "anon:\n");
    return true;
}

struct pfile {
    char *buf;
    size_t len;
};

static int64_t pfile_read(struct file *f, void *buf, uint64_t len)
{
    struct pfile *pf = f->priv;
    if (f->offset >= pf->len)
        return 0;
    if (len > pf->len - f->offset)
        len = pf->len - f->offset;
    memcpy(buf, pf->buf + f->offset, len);
    f->offset += len;
    return (int64_t)len;
}

static void pfile_close(struct file *f)
{
    struct pfile *pf = f->priv;
    kfree(pf->buf);
    kfree(pf);
}

static const struct file_ops pfile_ops = {
    .read = pfile_read,
    .close = pfile_close,
};

static int proc_open(struct vnode *v, int flags, struct file **out)
{
    (void)flags;
    enum pkind kind = key_kind(v);
    struct pbuf b = { 0 };
    struct process *p = NULL;
    if (kind >= P_PIDDIR) {
        p = process_get(key_pid(v));
        if (!p)
            return -ENOENT;
    }
    bool gone = false;
    switch (kind) {
    case P_MEMINFO: gen_meminfo(&b); break;
    case P_CPUINFO: gen_cpuinfo(&b); break;
    case P_UPTIME:  gen_uptime(&b); break;
    case P_MOUNTS:  gen_mounts(&b); break;
    case P_LOADAVG: gen_loadavg(&b); break;
    case P_VERSION: gen_version(&b); break;
    case P_DISKSTATS: gen_diskstats(&b); break;
    case P_SYSSTAT: gen_sysstat(&b); break;
    case P_STATUS:  gen_status(&b, p); break;
    case P_STAT:    gen_stat(&b, p); break;
    case P_CMDLINE: gen_cmdline(&b, p); break;
    case P_MAPS:    gen_maps(&b, p); break;
    case P_FD:      gone = !gen_fd(&b, p, key_fd(v)); break;
    default:        b.failed = true; break;
    }
    process_put(p);
    struct pfile *pf = b.failed || gone ? NULL : kzalloc(sizeof(*pf));
    struct file *f = pf ? file_alloc(&pfile_ops, pf) : NULL;
    if (!f) {
        kfree(pf);
        kfree(b.buf);
        return gone ? -ENOENT : -ENOMEM;
    }
    pf->buf = b.buf;
    pf->len = b.len;
    *out = f;
    return 0;
}

static int proc_mount(struct mount *m, struct blockdev *dev, const char *data)
{
    (void)dev;
    (void)data;
    struct vnode *root = vnode_new(m, mkkey(0, P_ROOT, 0), kind_mode(P_ROOT));
    if (!root)
        return -ENOMEM;
    m->root = vcache_insert(root);
    m->flags |= MNT_RDONLY;
    return 0;
}

const struct fs_type procfs_type = {
    .name = "proc",
    .needs_dev = false,
    .mount = proc_mount,
    .lookup = proc_lookup,
    .readdir = proc_readdir,
    .open = proc_open,
};

void procfs_init(void)
{
    if (!vfs_mounted())
        return;
    int r = vfs_mount_kernel("proc", NULL, "proc", "/proc");
    if (r < 0)
        log_warn("procfs", "nao foi possivel montar em /proc (erro %d)", -r);
    else
        log_info("procfs", "/proc montado");
}
