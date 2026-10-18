#include <kernel/process.h>
#include <kernel/futex.h>
#include <kernel/sched.h>
#include <kernel/elf.h>
#include <kernel/log.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <arch/x86_64/fpu.h>
#include <lib/random.h>
#include <mm/uaccess.h>
#include <mm/vma.h>
#include <kernel/smp.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

extern void user_return(struct interrupt_frame *frame);

static void kick_other_threads(struct process *p);
static bool become_single_thread(struct process *p);

static struct process *processes;
static uint32_t next_pid = 1;
static spinlock_t proc_lock = SPINLOCK_INIT;
static struct waitqueue child_exit_wq = WAITQUEUE_INIT;

static bool map_user_range(struct process *p, uint64_t pml4, uint64_t virt, uint64_t len, uint64_t flags)
{
    uint64_t start = PAGE_ALIGN_DOWN(virt);
    uint64_t end = PAGE_ALIGN_UP(virt + len);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (vmm_virt_to_phys_in(pml4, v))
            continue;
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return false;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        vmm_map_page_in(pml4, v, phys, flags);
        p->pages++;
    }
    return true;
}

static void copy_to_space(uint64_t pml4, uint64_t virt, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys = vmm_virt_to_phys_in(pml4, virt);
        size_t off = virt & (PAGE_SIZE - 1);
        size_t chunk = MIN(len, PAGE_SIZE - off);
        memcpy((uint8_t *)phys_to_virt(phys), s, chunk);
        virt += chunk;
        s += chunk;
        len -= chunk;
    }
}

struct exec_info {
    uint64_t entry;
    uint64_t phdr;
    uint64_t phnum;
    uint64_t phent;
    uint64_t brk;
    const void *phdr_src;
};

static bool load_elf(struct process *p, uint64_t pml4, const uint8_t *image, size_t size,
                     struct exec_info *info)
{
    if (size < sizeof(struct elf64_ehdr))
        return false;
    const struct elf64_ehdr *eh = (const void *)image;
    if (memcmp(eh->e_ident, ELF_MAGIC, 4) != 0 || eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB || eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return false;
    if (eh->e_phentsize < sizeof(struct elf64_phdr) || eh->e_phoff > size ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > size - eh->e_phoff)
        return false;

    uint64_t highest = 0;
    uint64_t phdr = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_PHDR)
            phdr = ph->p_vaddr;
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_memsz > USER_STACK_TOP || ph->p_vaddr > USER_STACK_TOP - ph->p_memsz ||
            ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_MAX ||
            ph->p_filesz > ph->p_memsz || ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
            return false;
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W)
            flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X))
            flags |= PTE_NX;
        if (!map_user_range(p, pml4, ph->p_vaddr, ph->p_memsz, flags))
            return false;
        uint32_t vf = VMA_READ | VMA_IMAGE;
        if (ph->p_flags & PF_W)
            vf |= VMA_WRITE;
        if (ph->p_flags & PF_X)
            vf |= VMA_EXEC;
        if (!vma_add_merge(p, pml4, PAGE_ALIGN_DOWN(ph->p_vaddr), PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz), vf))
            return false;
        copy_to_space(pml4, ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
        if (!phdr && eh->e_phoff >= ph->p_offset && eh->e_phoff < ph->p_offset + ph->p_filesz)
            phdr = ph->p_vaddr + (eh->e_phoff - ph->p_offset);
        if (ph->p_vaddr + ph->p_memsz > highest)
            highest = ph->p_vaddr + ph->p_memsz;
    }
    if (eh->e_entry >= USER_STACK_TOP)
        return false;
    info->entry = eh->e_entry;
    info->phdr = phdr;
    info->phnum = eh->e_phnum;
    info->phent = eh->e_phentsize;
    info->brk = PAGE_ALIGN_UP(highest);
    info->phdr_src = image + eh->e_phoff;
    return true;
}

#define AUX_WORDS 38

static uint64_t push_bytes(uint64_t pml4, uint64_t *sp, const void *data, size_t n)
{
    *sp -= n;
    copy_to_space(pml4, *sp, data, n);
    return *sp;
}

static uint64_t build_stack(uint64_t pml4, const char *const *argv, const char *const *envp,
                            const struct exec_info *info, const char *execfn)
{
    uint64_t argc = 0, envc = 0;
    uint64_t strings_size = 0;
    while (argv && argv[argc])
        strings_size += strlen(argv[argc++]) + 1;
    while (envp && envp[envc])
        strings_size += strlen(envp[envc++]) + 1;
    strings_size += strlen(execfn) + 1 + sizeof("x86_64") + 16 + 16;
    if (!info->phdr)
        strings_size += info->phnum * info->phent;

    uint64_t nwords = 1 + (argc + 1) + (envc + 1) + AUX_WORDS;
    if (strings_size + nwords * 8 + 64 > USER_STACK_SIZE / 2)
        return 0;
    uint64_t *vec = kzalloc(nwords * 8);
    if (!vec)
        return 0;

    uint64_t sp = USER_STACK_TOP;
    uint64_t fn_addr = push_bytes(pml4, &sp, execfn, strlen(execfn) + 1);
    uint64_t plat_addr = push_bytes(pml4, &sp, "x86_64", sizeof("x86_64"));
    uint8_t rnd[16];
    random_fill(rnd, sizeof(rnd));
    sp &= ~0xFull;
    uint64_t rnd_addr = push_bytes(pml4, &sp, rnd, sizeof(rnd));
    uint64_t phdr_addr = info->phdr;
    if (!phdr_addr) {
        phdr_addr = push_bytes(pml4, &sp, info->phdr_src, info->phnum * info->phent);
        phdr_addr &= ~0x7ull;
        sp = phdr_addr;
        copy_to_space(pml4, phdr_addr, info->phdr_src, info->phnum * info->phent);
    }

    uint64_t w = 0;
    vec[w++] = argc;
    for (uint64_t i = 0; i < argc; i++)
        vec[w++] = push_bytes(pml4, &sp, argv[i], strlen(argv[i]) + 1);
    vec[w++] = 0;
    for (uint64_t i = 0; i < envc; i++)
        vec[w++] = push_bytes(pml4, &sp, envp[i], strlen(envp[i]) + 1);
    vec[w++] = 0;

    struct cpuid_result id = cpuid(1, 0);
    uint64_t aux[][2] = {
        { AT_PHDR, phdr_addr },     { AT_PHENT, info->phent },  { AT_PHNUM, info->phnum },
        { AT_PAGESZ, PAGE_SIZE },    { AT_BASE, 0 },             { AT_FLAGS, 0 },
        { AT_ENTRY, info->entry },   { AT_UID, 0 },              { AT_EUID, 0 },
        { AT_GID, 0 },               { AT_EGID, 0 },             { AT_SECURE, 0 },
        { AT_CLKTCK, 100 },          { AT_HWCAP, id.edx },       { AT_PLATFORM, plat_addr },
        { AT_RANDOM, rnd_addr },     { AT_EXECFN, fn_addr },     { AT_NULL, 0 },
    };
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) {
        vec[w++] = aux[i][0];
        vec[w++] = aux[i][1];
    }

    sp &= ~0xFull;
    if (w & 1)
        sp -= 8;
    sp -= w * 8;
    copy_to_space(pml4, sp, vec, w * 8);
    kfree(vec);
    return sp;
}

static bool setup_image(struct process *p, uint64_t pml4, const void *image, size_t size,
                        const char *const *argv, const char *const *envp, const char *execfn)
{
    struct exec_info info;
    if (!load_elf(p, pml4, image, size, &info))
        return false;
    if (!map_user_range(p, pml4, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX))
        return false;
    if (!vma_add(p, USER_STACK_TOP - USER_STACK_MAX, USER_STACK_TOP, VMA_READ | VMA_WRITE | VMA_STACK,
                 NULL, 0, 0))
        return false;
    uint64_t sp = build_stack(pml4, argv, envp, &info, execfn);
    if (!sp)
        return false;
    p->entry = info.entry;
    p->stack_top = sp;
    p->brk_start = info.brk;
    p->brk = info.brk;
    return true;
}

static void enter_user_mode(uint64_t entry, uint64_t user_rsp)
{
    cli();
    __asm__ volatile(
        "pushq %[ss]\n\t"
        "pushq %[rsp]\n\t"
        "pushq $0x202\n\t"
        "pushq %[cs]\n\t"
        "pushq %[rip]\n\t"
        "swapgs\n\t"
        "iretq"
        :
        : [ss] "i"(GDT_USER_DATA | GDT_RPL_USER), [rsp] "r"(user_rsp),
          [cs] "i"(GDT_USER_CODE | GDT_RPL_USER), [rip] "r"(entry)
        : "memory");
    __builtin_unreachable();
}

static void user_thread_entry(void *arg)
{
    struct process *p = arg;
    enter_user_mode(p->entry, p->stack_top);
}

static void process_free_now(struct process *p)
{
    vma_release_all(p);
    vnode_put(p->cwd_vn);
    kfree(p->cmdline);
    kfree(p->sig_handlers);
    kfree(p->fds);
    kfree(p->fd_flags);
    kfree(p);
}

static void process_free(struct process *p)
{
    if (!p)
        return;
    __atomic_store_n(&p->free_pending, true, __ATOMIC_SEQ_CST);
    bool expect = true;
    if (__atomic_load_n(&p->pins, __ATOMIC_SEQ_CST) == 0 &&
        __atomic_compare_exchange_n(&p->free_pending, &expect, false, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        process_free_now(p);
}

void process_put(struct process *p)
{
    if (!p)
        return;
    bool expect = true;
    if (__atomic_sub_fetch(&p->pins, 1, __ATOMIC_SEQ_CST) == 0 &&
        __atomic_compare_exchange_n(&p->free_pending, &expect, false, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        process_free_now(p);
}

static void set_name(char *dst, size_t size, const char *name)
{
    strlcpy(dst, name, size);
    for (char *c = dst; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7F || *c == '(' || *c == ')')
            *c = '?';
}

static char *join_argv(const char *const *argv, uint32_t *len_out)
{
    uint32_t len = 0;
    for (int i = 0; argv && argv[i]; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        if (len + n > PROCESS_CMDLINE_MAX)
            break;
        len += n;
    }
    char *buf = kmalloc(len ? len : 1);
    if (!buf)
        return NULL;
    uint32_t at = 0;
    for (int i = 0; argv && argv[i]; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]) + 1;
        if (at + n > len)
            break;
        memcpy(buf + at, argv[i], n);
        at += n;
    }
    *len_out = at;
    return buf;
}

static void set_cmdline(struct process *p, char *cmd, uint32_t len)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    char *old = p->cmdline;
    p->cmdline = cmd;
    p->cmdline_len = cmd ? len : 0;
    spin_unlock_irqrestore(&p->tlock, flags);
    kfree(old);
}

uint32_t process_cmdline(struct process *p, char *out, uint32_t size)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    uint32_t n = p->cmdline_len < size ? p->cmdline_len : size;
    if (p->cmdline)
        memcpy(out, p->cmdline, n);
    else
        n = 0;
    spin_unlock_irqrestore(&p->tlock, flags);
    return n;
}

static void rlimits_default(struct process *p)
{
    for (int i = 0; i < RLIMIT_COUNT; i++) {
        p->rlimits[i].cur = RLIM_INFINITY;
        p->rlimits[i].max = RLIM_INFINITY;
    }
    p->rlimits[RLIMIT_NOFILE].cur = p->rlimits[RLIMIT_NOFILE].max = PROCESS_FD_MAX;
    p->rlimits[RLIMIT_NPROC].cur = p->rlimits[RLIMIT_NPROC].max = PROCESS_NPROC_MAX;
    p->rlimits[RLIMIT_STACK].cur = p->rlimits[RLIMIT_STACK].max = USER_STACK_SIZE;
}

static struct process *process_alloc(const char *name)
{
    struct process *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->fds = kzalloc(PROCESS_FD_INITIAL * sizeof(struct file *));
    p->fd_flags = kzalloc(PROCESS_FD_INITIAL);
    p->sig_handlers = kzalloc(SIG_TABLE_SIZE);
    if (!p->fds || !p->fd_flags || !p->sig_handlers) {
        process_free(p);
        return NULL;
    }
    p->fd_cap = PROCESS_FD_INITIAL;
    rlimits_default(p);
    p->umask = 022;
    spinlock_init(&p->cred_lock);
    strlcpy(p->name, name, sizeof(p->name));
    p->pml4_phys = vmm_create_address_space();
    if (!p->pml4_phys) {
        process_free(p);
        return NULL;
    }
    p->state = PROC_RUNNING;
    spinlock_init(&p->tlock);
    mutex_init_private(&p->mm_lock, "mm");
    signal_init_process(p);
    strlcpy(p->cwd, "/", sizeof(p->cwd));
    return p;
}

void process_get_cwd(struct process *p, char *out, size_t size)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    strlcpy(out, p->cwd, size);
    spin_unlock_irqrestore(&p->tlock, flags);
}

void process_set_cwd(struct process *p, const char *path, struct vnode *v)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct vnode *old = p->cwd_vn;
    p->cwd_vn = v;
    strlcpy(p->cwd, path, sizeof(p->cwd));
    spin_unlock_irqrestore(&p->tlock, flags);
    vnode_put(old);
}

void process_update_cwd_path(struct process *p, struct vnode *v, const char *path)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (p->cwd_vn == v)
        strlcpy(p->cwd, path, sizeof(p->cwd));
    spin_unlock_irqrestore(&p->tlock, flags);
}

struct vnode *process_cwd_vnode(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct vnode *v = p->cwd_vn;
    if (v)
        vnode_get(v);
    spin_unlock_irqrestore(&p->tlock, flags);
    return v ? v : vfs_root();
}

bool process_resolve_path(struct process *p, const char *path, char *out, size_t out_size)
{
    char tmp[PROCESS_CWD_MAX * 2];
    if (path[0] == '/') {
        if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp))
            return false;
    } else {
        char cwd[PROCESS_CWD_MAX];
        process_get_cwd(p, cwd, sizeof(cwd));
        if (!cwd[0] || strlen(cwd) + 1 + strlen(path) >= sizeof(tmp))
            return false;
        ksnprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }

    size_t o = 0;
    out[o++] = '/';
    const char *s = tmp;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        const char *start = s;
        while (*s && *s != '/')
            s++;
        size_t n = (size_t)(s - start);
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (o > 1) {
                o--;
                while (o > 1 && out[o - 1] != '/')
                    o--;
            }
            continue;
        }
        if (o + n + 1 >= out_size)
            return false;
        if (out[o - 1] != '/')
            out[o++] = '/';
        memcpy(out + o, start, n);
        o += n;
    }
    if (o > 1 && out[o - 1] == '/')
        o--;
    out[o] = '\0';
    return true;
}

static void process_register(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    p->pid = next_pid++;
    if (!p->pgid)
        p->pgid = p->pid;
    p->next = processes;
    processes = p;
    spin_unlock_irqrestore(&proc_lock, flags);
}

static void orphan_children_locked(struct process *p)
{
    struct process **pp = &processes;
    while (*pp) {
        struct process *c = *pp;
        if (c->parent == p) {
            c->parent = NULL;
            c->autofree = c->state == PROC_ZOMBIE;
            if (c->state == PROC_ZOMBIE && c->pml4_phys == 0) {
                *pp = c->next;
                process_free(c);
                continue;
            }
        }
        pp = &c->next;
    }
}

static void unregister_locked(struct process *p)
{
    struct process **pp = &processes;
    while (*pp && *pp != p)
        pp = &(*pp)->next;
    if (*pp)
        *pp = p->next;
    orphan_children_locked(p);
    p->parent = NULL;
}

static void process_unregister(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    unregister_locked(p);
    spin_unlock_irqrestore(&proc_lock, flags);
}

static void give_std_fds(struct process *p)
{
    for (int i = 0; i < 3; i++)
        p->fds[i] = file_open_tty();
}

static void link_and_start(struct process *p, struct thread *t)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    t->proc_next = p->threads;
    p->threads = t;
    p->live_threads++;
    __atomic_fetch_add(&p->thread_refs, 1, __ATOMIC_ACQ_REL);
    if (p->group_exit || p->execing)
        t->kill_me = true;
    sched_runq_lock();
    sched_make_ready(t);
    sched_runq_unlock();
    spin_unlock_irqrestore(&p->tlock, flags);
}

static uint32_t alloc_id(void)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    uint32_t id = next_pid++;
    spin_unlock_irqrestore(&proc_lock, flags);
    return id;
}

static struct process *process_start(struct process *p, const char *name)
{
    process_register(p);
    struct thread *t = thread_create_for_process_paused(name, user_thread_entry, p, p);
    if (!t) {
        process_unregister(p);
        vmm_destroy_address_space(p->pml4_phys);
        process_free(p);
        return NULL;
    }
    t->utid = p->pid;
    link_and_start(p, t);
    return p;
}

struct process *process_create_elf(const char *name, const void *image, size_t size)
{
    struct process *p = process_alloc(name);
    if (!p)
        return NULL;
    const char *argv[] = { name, NULL };
    if (!setup_image(p, p->pml4_phys, image, size, argv, NULL, name)) {
        log_error("proc", "'%s': ELF invalido", name);
        vmm_destroy_address_space(p->pml4_phys);
        process_free(p);
        return NULL;
    }
    give_std_fds(p);
    p->cwd_vn = vfs_root();
    uint32_t clen;
    p->cmdline = join_argv(argv, &clen);
    p->cmdline_len = p->cmdline ? clen : 0;
    p->autofree = true;
    return process_start(p, name);
}

struct process *process_spawn_path(const char *path, const char *const *argv)
{
    uint64_t size;
    void *image = vfs_read_all(path, &size);
    if (!image)
        return NULL;
    const char *name = path;
    for (const char *c = path; *c; c++)
        if (*c == '/')
            name = c + 1;
    struct process *p = process_alloc(name);
    if (!p) {
        kfree(image);
        return NULL;
    }
    const char *def[] = { name, NULL };
    bool ok = setup_image(p, p->pml4_phys, image, size, argv ? argv : def, NULL, path);
    kfree(image);
    if (!ok) {
        vmm_destroy_address_space(p->pml4_phys);
        process_free(p);
        return NULL;
    }
    give_std_fds(p);
    p->cwd_vn = vfs_root();
    uint32_t clen;
    p->cmdline = join_argv(argv ? argv : def, &clen);
    strlcpy(p->exe, path, sizeof(p->exe));
    p->cmdline_len = p->cmdline ? clen : 0;
    p->autofree = true;
    return process_start(p, name);
}

struct process *process_current(void)
{
    return thread_current()->process;
}

struct process *process_find(uint32_t pid)
{
    for (struct process *p = processes; p; p = p->next)
        if (p->pid == pid)
            return p;
    return NULL;
}

struct process *process_get(uint32_t pid)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    struct process *p = process_find(pid);
    if (p)
        __atomic_add_fetch(&p->pins, 1, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&proc_lock, flags);
    return p;
}

int process_pids(uint32_t *out, int max)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int n = 0;
    for (struct process *p = processes; p && n < max; p = p->next)
        out[n++] = p->pid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return n;
}

uint32_t process_ppid(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    uint32_t ppid = p->parent ? p->parent->pid : 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    return ppid;
}

int process_fd_limit(struct process *p)
{
    uint64_t lim = p->rlimits[RLIMIT_NOFILE].cur;
    return lim > PROCESS_FD_MAX ? PROCESS_FD_MAX : (int)lim;
}

static bool fd_table_resize(struct process *p, int cap)
{
    if (cap <= p->fd_cap)
        return true;
    struct file **n = kzalloc((size_t)cap * sizeof(struct file *));
    uint8_t *fl = kzalloc((size_t)cap);
    if (!n || !fl) {
        kfree(n);
        kfree(fl);
        return false;
    }
    memcpy(n, p->fds, (size_t)p->fd_cap * sizeof(struct file *));
    memcpy(fl, p->fd_flags, (size_t)p->fd_cap);
    kfree(p->fds);
    kfree(p->fd_flags);
    p->fds = n;
    p->fd_flags = fl;
    p->fd_cap = cap;
    return true;
}

bool process_fd_reserve(struct process *p, int fd)
{
    if (fd < 0 || fd >= process_fd_limit(p))
        return false;
    if (fd < p->fd_cap)
        return true;
    int cap = p->fd_cap;
    while (cap <= fd)
        cap *= 2;
    if (cap > PROCESS_FD_MAX)
        cap = PROCESS_FD_MAX;
    return fd_table_resize(p, cap);
}

int process_fd_alloc(struct process *p, struct file *f, int min, bool cloexec)
{
    int limit = process_fd_limit(p);
    if (min < 0)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    int ret = -EMFILE;
    for (int i = min; i < limit; i++) {
        if (i >= p->fd_cap && !process_fd_reserve(p, i))
            break;
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->fd_flags[i] = cloexec ? FD_CLOEXEC : 0;
            ret = i;
            break;
        }
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    return ret;
}

int process_fd_install(struct process *p, struct file *f)
{
    return process_fd_alloc(p, f, 0, false);
}

int process_fd_replace(struct process *p, int fd, struct file *f, bool cloexec)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (!process_fd_reserve(p, fd)) {
        spin_unlock_irqrestore(&p->tlock, flags);
        return -EBADF;
    }
    struct file *old = p->fds[fd];
    p->fds[fd] = f;
    p->fd_flags[fd] = cloexec ? FD_CLOEXEC : 0;
    spin_unlock_irqrestore(&p->tlock, flags);
    if (old)
        file_unref(old);
    return fd;
}

int process_fd_get_flags(struct process *p, int fd)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    int ret = process_fd_get(p, fd) ? p->fd_flags[fd] : -EBADF;
    spin_unlock_irqrestore(&p->tlock, flags);
    return ret;
}

int process_fd_set_flags(struct process *p, int fd, int value)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    int ret = -EBADF;
    if (process_fd_get(p, fd)) {
        p->fd_flags[fd] = (uint8_t)(value & FD_CLOEXEC);
        ret = 0;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    return ret;
}

static void close_on_exec(struct process *p)
{
    for (int i = 0; i < p->fd_cap; i++)
        if (p->fds[i] && (p->fd_flags[i] & FD_CLOEXEC))
            process_fd_close(p, i);
}

struct file *process_fd_get(struct process *p, int fd)
{
    if (fd < 0 || fd >= p->fd_cap)
        return NULL;
    return p->fds[fd];
}

void process_fd_close_if(struct process *p, int fd, struct file *f)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    bool mine = fd >= 0 && fd < p->fd_cap && p->fds[fd] == f;
    if (mine) {
        p->fds[fd] = NULL;
        p->fd_flags[fd] = 0;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    if (mine)
        file_unref(f);
}

int process_fd_close(struct process *p, int fd)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct file *f = process_fd_get(p, fd);
    if (f) {
        p->fds[fd] = NULL;
        p->fd_flags[fd] = 0;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    if (!f)
        return -EBADF;
    file_unref(f);
    return 0;
}

static bool share_user_pages(struct process *child, struct process *parent)
{
    uint64_t *pml4 = phys_to_virt(parent->pml4_phys);
    for (uint64_t i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *pdpt = phys_to_virt(pml4[i4] & PTE_ADDR_MASK);
        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            uint64_t *pd = phys_to_virt(pdpt[i3] & PTE_ADDR_MASK);
            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                uint64_t *pt = phys_to_virt(pd[i2] & PTE_ADDR_MASK);
                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    uint64_t e = pt[i1];
                    if (!(e & (PTE_PRESENT | PTE_PROTNONE))) continue;
                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    uint64_t phys = e & PTE_ADDR_MASK;
                    uint64_t flags = e & ~PTE_ADDR_MASK;
                    if (e & PTE_NOFREE) {
                        vmm_set_pte_in(child->pml4_phys, virt, e);
                        continue;
                    }
                    struct vma *v = vma_find(parent, virt);
                    bool shared = v && (v->flags & VMA_SHARED);
                    if (!shared) {
                        flags = (flags & ~PTE_WRITABLE) | PTE_COW;
                        pt[i1] = phys | flags;
                    }
                    pmm_ref_inc(phys);
                    vmm_set_pte_in(child->pml4_phys, virt, phys | flags);
                    child->pages++;
                }
            }
        }
    }
    write_cr3(read_cr3());
    return true;
}

struct fork_start {
    struct interrupt_frame frame;
    uint8_t fpu[FPU_STATE_SIZE] ALIGNED(16);
};

static void fork_child_entry(void *arg)
{
    struct fork_start *st = arg;
    struct interrupt_frame frame = st->frame;
    if (thread_current()->kill_me) {
        kfree(st);
        process_thread_leave();
    }
    cli();
    fpu_restore(st->fpu);
    kfree(st);
    user_return(&frame);
}

int process_fork(struct interrupt_frame *frame)
{
    return process_fork_stack(frame, 0);
}

int process_fork_stack(struct interrupt_frame *frame, uint64_t newsp)
{
    struct process *parent = process_current();
    if ((uint64_t)process_count() >= parent->rlimits[RLIMIT_NPROC].cur)
        return -EAGAIN;
    struct pmm_stats mem;
    pmm_get_stats(&mem);
    if (mem.free_pages < mem.total_pages / 16)
        vfs_cache_reclaim(mem.total_pages / 16);
    struct process *child = process_alloc(parent->name);
    if (!child)
        return -ENOMEM;

    mutex_lock(&parent->mm_lock);
    bool copied = vma_copy_all(child, parent) && share_user_pages(child, parent);
    process_mm_tlb_sync(parent);
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    mutex_unlock(&parent->mm_lock);
    if (!copied) {
        vma_release_all(child);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    child->pgid = parent->pgid;
    signal_copy_handlers(child, parent);
    child->parent = parent;
    memcpy(child->rlimits, parent->rlimits, sizeof(child->rlimits));
    child->umask = __atomic_load_n(&parent->umask, __ATOMIC_RELAXED);
    cred_get(&child->cred);
    uint64_t tflags = spin_lock_irqsave(&parent->tlock);
    strlcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->cwd_vn = parent->cwd_vn;
    if (child->cwd_vn)
        vnode_get(child->cwd_vn);
    strlcpy(child->exe, parent->exe, sizeof(child->exe));
    child->cmdline = parent->cmdline ? kmalloc(parent->cmdline_len ? parent->cmdline_len : 1) : NULL;
    if (child->cmdline) {
        memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
        child->cmdline_len = parent->cmdline_len;
    }
    if (!fd_table_resize(child, parent->fd_cap)) {
        spin_unlock_irqrestore(&parent->tlock, tflags);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    for (int i = 0; i < parent->fd_cap; i++) {
        child->fds[i] = parent->fds[i];
        child->fd_flags[i] = parent->fd_flags[i];
        if (child->fds[i])
            file_ref(child->fds[i]);
    }
    spin_unlock_irqrestore(&parent->tlock, tflags);

    struct fork_start *copy = kmalloc(sizeof(*copy));
    if (!copy) {
        for (int i = 0; i < child->fd_cap; i++)
            if (child->fds[i])
                process_fd_close(child, i);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    copy->frame = *frame;
    copy->frame.rax = 0;
    if (newsp)
        copy->frame.rsp = newsp;
    fpu_save(copy->fpu);

    process_register(child);
    struct thread *t = thread_create_for_process_paused(child->name, fork_child_entry, copy, child);
    if (!t) {
        process_unregister(child);
        kfree(copy);
        for (int i = 0; i < child->fd_cap; i++)
            if (child->fds[i])
                process_fd_close(child, i);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    struct thread *self = thread_current();
    int pid = (int)child->pid;
    t->utid = child->pid;
    t->sig_blocked = self->sig_blocked;
    t->fs_base = self->fs_base;
    link_and_start(child, t);
    return pid;
}

long process_clone(struct interrupt_frame *frame, uint64_t flags, uint64_t newsp, uint64_t ptid,
                   uint64_t ctid, uint64_t tls)
{
    if (!(flags & CLONE_THREAD))
        return process_fork_stack(frame, newsp);
    if ((flags & (CLONE_VM | CLONE_SIGHAND)) != (CLONE_VM | CLONE_SIGHAND) || (flags & 0xFF))
        return -EINVAL;
    struct process *p = process_current();
    struct thread *self = thread_current();
    if (p->group_exit)
        return -EAGAIN;
    struct fork_start *st = kmalloc(sizeof(*st));
    if (!st)
        return -ENOMEM;
    st->frame = *frame;
    st->frame.rax = 0;
    if (newsp)
        st->frame.rsp = newsp;
    fpu_save(st->fpu);
    struct thread *t = thread_create_for_process_paused(self->name, fork_child_entry, st, p);
    if (!t) {
        kfree(st);
        return -EAGAIN;
    }
    uint32_t tid = alloc_id();
    t->utid = tid;
    t->sig_blocked = self->sig_blocked;
    t->fs_base = (flags & CLONE_SETTLS) ? tls : self->fs_base;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid;
    if (flags & CLONE_PARENT_SETTID)
        copy_to_user((void *)ptid, &tid, sizeof(tid));
    if (flags & CLONE_CHILD_SETTID)
        copy_to_user((void *)ctid, &tid, sizeof(tid));
    link_and_start(p, t);
    return (long)tid;
}

int process_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *frame)
{
    struct process *p = process_current();
    struct vnode *start = path[0] == '/' ? NULL : process_cwd_vnode(p);
    struct vnode *v;
    int r = vfs_walk(start, path, &v);
    vnode_put(start);
    if (r < 0)
        return r;
    if (vnode_is_reg(v))
        r = vfs_permission(v, VFS_MAY_EXEC);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    struct vattr va;
    vfs_getattr(v, &va);
    char full[PROCESS_CWD_MAX];
    if (!process_resolve_path(p, path, full, sizeof(full)))
        strlcpy(full, path, sizeof(full));
    if (vnode_is_reg(v) && v->size > VFS_READ_ALL_MAX) {
        vnode_put(v);
        return -ENOEXEC;
    }
    uint64_t size;
    void *image = vnode_is_reg(v) ? vfs_read_vnode(v, &size) : NULL;
    bool regular = vnode_is_reg(v);
    vnode_put(v);
    if (!image)
        return regular ? -EIO : -EACCES;
    if (!become_single_thread(p)) {
        kfree(image);
        return -EAGAIN;
    }
    path = full;

    uint32_t clen = 0;
    char *cmd = join_argv((const char *const *)argv, &clen);
    mutex_lock(&p->mm_lock);
    uint64_t old_pml4 = p->pml4_phys;
    uint64_t old_pages = p->pages, old_entry = p->entry;
    struct vma *old_vmas = p->vmas;
    p->vmas = NULL;
    uint64_t old_stack = p->stack_top, old_brk_start = p->brk_start, old_brk = p->brk;
    uint64_t new_pml4 = vmm_create_address_space();
    p->pages = 0;
    int ret = 0;
    if (!new_pml4 ||
        !setup_image(p, new_pml4, image, size, (const char *const *)argv, (const char *const *)envp, full)) {
        if (new_pml4)
            vmm_destroy_address_space(new_pml4);
        p->pages = old_pages;
        vma_release_all(p);
        p->vmas = old_vmas;
        p->entry = old_entry;
        p->stack_top = old_stack;
        p->brk_start = old_brk_start;
        p->brk = old_brk;
        ret = -ENOEXEC;
        mutex_unlock(&p->mm_lock);
        kfree(cmd);
    } else {
        const char *name = path;
        for (const char *c = path; *c; c++)
            if (*c == '/')
                name = c + 1;
        set_name(p->name, sizeof(p->name), name);
        set_name(thread_current()->name, THREAD_NAME_MAX, name);
        signal_reset_on_exec(p);
        thread_current()->fs_base = 0;
        wrmsr(MSR_IA32_FS_BASE, 0);
        fpu_reset_current();
        thread_current()->clear_child_tid = 0;
        thread_current()->utid = p->pid;
        close_on_exec(p);
        cred_exec(va.mode, va.uid, va.gid);
        cli();
        p->pml4_phys = new_pml4;
        write_cr3(new_pml4);
        sti();
        vmm_destroy_address_space(old_pml4);
        struct vma *new_vmas = p->vmas;
        p->vmas = old_vmas;
        vma_release_all(p);
        p->vmas = new_vmas;
        strlcpy(p->exe, full, sizeof(p->exe));
        mutex_unlock(&p->mm_lock);
        set_cmdline(p, cmd, clen);

        memset(frame, 0, offsetof(struct interrupt_frame, rip));
        frame->rip = p->entry;
        frame->rcx = p->entry;
        frame->rsp = p->stack_top;
        frame->rflags = 0x202;
        frame->r11 = 0x202;
        frame->cs = GDT_USER_CODE | GDT_RPL_USER;
        frame->ss = GDT_USER_DATA | GDT_RPL_USER;
    }

    kfree(image);
    return ret;
}

int process_waitpid(int pid, int *status, int options, struct cpu_times *child)
{
    struct process *me = process_current();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (;;) {
        bool have_child = false;
        struct process *found = NULL;
        for (struct process *c = processes; c; c = c->next) {
            if (c->parent != me)
                continue;
            if (pid > 0 && (int)c->pid != pid)
                continue;
            have_child = true;
            if (c->state == PROC_ZOMBIE && c->pml4_phys == 0) {
                found = c;
                break;
            }
        }
        if (found) {
            int rpid = (int)found->pid;
            if (status)
                *status = found->exit_code;
            uint64_t cu = found->dead_utime + found->child_utime;
            uint64_t cs = found->dead_stime + found->child_stime;
            me->child_utime += cu;
            me->child_stime += cs;
            if (child) {
                child->utime = cu;
                child->stime = cs;
            }
            unregister_locked(found);
            spin_unlock_irqrestore(&proc_lock, flags);
            process_free(found);
            return rpid;
        }
        if (!have_child) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return 0;
        }
        wait_sleep_intr(&child_exit_wq, &proc_lock, 0);
        if (signal_pending_current()) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return -ERESTARTSYS;
        }
    }
}

static void kick_other_threads(struct process *p)
{
    struct thread *self = thread_current();
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (t == self)
            continue;
        t->kill_me = true;
        sched_runq_lock();
        wait_unlink(t);
        sched_make_ready(t);
        sched_runq_unlock();
    }
    spin_unlock_irqrestore(&p->tlock, flags);
}

static bool become_single_thread(struct process *p)
{
    if (__atomic_load_n(&p->thread_refs, __ATOMIC_ACQUIRE) <= 1)
        return true;
    struct thread *self = thread_current();
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (p->group_exit || p->execing || self->kill_me) {
        spin_unlock_irqrestore(&p->tlock, flags);
        return false;
    }
    p->execing = true;
    spin_unlock_irqrestore(&p->tlock, flags);
    kick_other_threads(p);
    bool ok = true;
    while (__atomic_load_n(&p->thread_refs, __ATOMIC_ACQUIRE) > 1) {
        if (self->kill_me || p->group_exit) {
            ok = false;
            break;
        }
        thread_sleep_ms(1);
    }
    flags = spin_lock_irqsave(&p->tlock);
    p->execing = false;
    spin_unlock_irqrestore(&p->tlock, flags);
    return ok;
}

static NORETURN void thread_leave(int status)
{
    struct process *p = process_current();
    ASSERT(p);
    struct thread *self = thread_current();
    if (self->clear_child_tid) {
        int32_t zero = 0;
        if (copy_to_user((void *)self->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake((uint32_t *)self->clear_child_tid, 1, false);
        self->clear_child_tid = 0;
    }
    process_unpin_all();

    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct thread **pp = &p->threads;
    while (*pp && *pp != self)
        pp = &(*pp)->proc_next;
    if (*pp)
        *pp = self->proc_next;
    uint64_t self_user, self_sys;
    bool last = --p->live_threads == 0;
    if (!last) {
        thread_cpu_times(self, &self_user, &self_sys);
        p->dead_utime += self_user;
        p->dead_stime += self_sys;
    }
    if (last && !p->group_exit) {
        p->group_exit = true;
        p->group_exit_status = status;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    if (!last) {
        signal_retarget(p, __atomic_load_n(&p->sig_pending, __ATOMIC_SEQ_CST));
        thread_exit();
    }

    p->exit_code = p->group_exit_status;
    p->alarm_at = 0;
    p->alarm_interval = 0;
    for (int i = 0; i < p->fd_cap; i++)
        if (p->fds[i])
            process_fd_close(p, i);
    mutex_lock(&p->mm_lock);
    vma_release_all(p);
    mutex_unlock(&p->mm_lock);
    while (p->free_n)
        pmm_free_page(p->free_batch[--p->free_n]);
    flags = spin_lock_irqsave(&p->tlock);
    struct vnode *cwd = p->cwd_vn;
    p->cwd_vn = NULL;
    spin_unlock_irqrestore(&p->tlock, flags);
    vnode_put(cwd);
    thread_cpu_times(self, &self_user, &self_sys);
    flags = spin_lock_irqsave(&p->tlock);
    p->dead_utime += self_user;
    p->dead_stime += self_sys;
    spin_unlock_irqrestore(&p->tlock, flags);
    flags = spin_lock_irqsave(&proc_lock);
    p->state = PROC_ZOMBIE;
    orphan_children_locked(p);
    if (!p->parent)
        p->autofree = true;
    spin_unlock_irqrestore(&proc_lock, flags);
    thread_exit();
}

static NORETURN void exit_group(int status)
{
    struct process *p = process_current();
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    if (!p->group_exit) {
        p->group_exit = true;
        p->group_exit_status = status;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    kick_other_threads(p);
    thread_leave(status);
}

NORETURN void process_thread_leave(void)
{
    thread_leave(0);
}

NORETURN void process_exit_thread(int code)
{
    thread_leave((code & 0xFF) << 8);
}

NORETURN void process_exit(int code)
{
    exit_group((code & 0xFF) << 8);
}

NORETURN void process_exit_signal(int sig)
{
    exit_group(sig & 0x7F);
}

struct file *process_fd_pin(int fd)
{
    struct thread *t = thread_current();
    struct process *p = t->process;
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    struct file *f = process_fd_get(p, fd);
    if (f) {
        bool have = false;
        for (int i = 0; i < t->npins; i++)
            if (t->pins[i] == f)
                have = true;
        if (!have) {
            if (t->npins >= THREAD_PINS) {
                spin_unlock_irqrestore(&p->tlock, flags);
                return NULL;
            }
            file_ref(f);
            t->pins[t->npins++] = f;
        }
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    return f;
}

void process_unpin_all(void)
{
    struct thread *t = thread_current();
    while (t->npins > 0) {
        struct file *f = t->pins[--t->npins];
        t->pins[t->npins] = NULL;
        file_unref(f);
    }
}

void process_mm_tlb_sync(struct process *p)
{
    if (p->live_threads > 1) {
        if (!interrupts_enabled()) {
            smp_flush_tlb_others();
            return;
        }
        smp_tlb_shootdown_sync();
    }
    while (p->free_n)
        pmm_free_page(p->free_batch[--p->free_n]);
}

void process_defer_free(struct process *p, uint64_t phys)
{
    if (p->live_threads <= 1) {
        pmm_free_page(phys);
        return;
    }
    if (p->free_n == PROCESS_FREE_BATCH)
        process_mm_tlb_sync(p);
    if (p->free_n == PROCESS_FREE_BATCH) {
        pmm_free_page(phys);
        return;
    }
    p->free_batch[p->free_n++] = phys;
}

static bool may_signal(const struct cred *me, struct process *target)
{
    if (me->euid == 0)
        return true;
    uint64_t flags = spin_lock_irqsave(&target->cred_lock);
    bool ok = me->uid == target->cred.uid || me->uid == target->cred.suid || me->euid == target->cred.uid ||
              me->euid == target->cred.suid;
    spin_unlock_irqrestore(&target->cred_lock, flags);
    return ok;
}

int process_kill(int pid, int sig)
{
    struct process *me = process_current();
    struct cred mc;
    cred_get(&mc);
    int found = 0, denied = 0;
    int err = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    uint32_t group = pid == 0 ? me->pgid : (uint32_t)(-pid);
    for (struct process *c = processes; c; c = c->next) {
        bool match = pid > 0 ? (int)c->pid == pid : c->pgid == group;
        if (!match || c->state == PROC_ZOMBIE)
            continue;
        if (!may_signal(&mc, c)) {
            denied++;
            continue;
        }
        found++;
        struct sig_src src = { me ? SI_USER : SI_KERNEL, 0, me ? me->pid : 0, mc.uid, 0 };
        int r = sig ? signal_send_info(c, sig, &src) : 0;
        if (r < 0)
            err = r;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    if (err)
        return err;
    if (!found && denied)
        return -EPERM;
    return found ? 0 : -ESRCH;
}

int process_setpgid(int pid, int pgid)
{
    struct process *me = process_current();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    struct process *target = me;
    if (pid > 0) {
        target = NULL;
        for (struct process *c = processes; c; c = c->next)
            if ((int)c->pid == pid)
                target = c;
    }
    int ret = 0;
    if (!target)
        ret = -ESRCH;
    else
        target->pgid = pgid > 0 ? (uint32_t)pgid : target->pid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return ret;
}

int process_getpgid(int pid)
{
    struct process *me = process_current();
    if (pid <= 0)
        return (int)me->pgid;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int ret = -ESRCH;
    for (struct process *c = processes; c; c = c->next)
        if ((int)c->pid == pid)
            ret = (int)c->pgid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return ret;
}

void process_cpu_times(struct process *p, struct cpu_times *self, struct cpu_times *children)
{
    uint64_t flags = spin_lock_irqsave(&p->tlock);
    self->utime = p->dead_utime;
    self->stime = p->dead_stime;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        uint64_t u, s;
        thread_cpu_times(t, &u, &s);
        self->utime += u;
        self->stime += s;
    }
    spin_unlock_irqrestore(&p->tlock, flags);
    flags = spin_lock_irqsave(&proc_lock);
    children->utime = p->child_utime;
    children->stime = p->child_stime;
    spin_unlock_irqrestore(&proc_lock, flags);
}

long process_signal_thread(int tgid, int tid, int sig)
{
    if (tid <= 0 || sig < 0 || sig >= NSIG)
        return -EINVAL;
    long ret = -ESRCH;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (struct process *c = processes; c; c = c->next) {
        if (c->state == PROC_ZOMBIE || (tgid > 0 && (int)c->pid != tgid))
            continue;
        uint64_t tf = spin_lock_irqsave(&c->tlock);
        struct thread *found = NULL;
        for (struct thread *t = c->threads; t; t = t->proc_next)
            if ((int)t->utid == tid) {
                found = t;
                break;
            }
        struct cred mc;
        cred_get(&mc);
        if (found && !may_signal(&mc, c)) {
            ret = -EPERM;
        } else if (found) {
            ret = 0;
            if (sig) {
                struct sig_src src = { SI_TKILL, 0, process_current()->pid, 0, 0 };
                signal_send_thread_info(found, sig, &src, true);
            }
        }
        spin_unlock_irqrestore(&c->tlock, tf);
        if (found)
            break;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return ret;
}

static struct waitqueue alarm_wq = WAITQUEUE_INIT;

static uint64_t ms_to_ticks(uint64_t ms)
{
    return ms * TIME_TICK_HZ / 1000;
}

static uint64_t ticks_to_ms(uint64_t t)
{
    return t * 1000 / TIME_TICK_HZ;
}

void process_itimer(bool set, uint64_t value_ms, uint64_t interval_ms, uint64_t *old_value, uint64_t *old_interval)
{
    struct process *p = process_current();
    uint64_t now = time_ticks();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    *old_value = p->alarm_at > now ? ticks_to_ms(p->alarm_at - now) : 0;
    if (p->alarm_at && *old_value == 0)
        *old_value = 1;
    *old_interval = ticks_to_ms(p->alarm_interval);
    if (set) {
        uint64_t value = ms_to_ticks(value_ms);
        uint64_t interval = ms_to_ticks(interval_ms);
        p->alarm_at = value_ms ? now + (value ? value : 1) : 0;
        p->alarm_interval = value_ms && interval_ms ? (interval ? interval : 1) : 0;
        wake_up_all(&alarm_wq);
    }
    spin_unlock_irqrestore(&proc_lock, flags);
}

long process_alarm(uint64_t seconds)
{
    uint64_t old_value, old_interval;
    if (seconds > 1000000000ull)
        seconds = 1000000000ull;
    process_itimer(true, seconds * 1000, 0, &old_value, &old_interval);
    uint64_t left = (old_value + 500) / 1000;
    if (old_value && !left)
        left = 1;
    return (long)left;
}

static void alarm_thread(void *arg)
{
    (void)arg;
    cli();
    spin_lock(&proc_lock);
    for (;;) {
        uint64_t now = time_ticks();
        uint64_t next = 0;
        for (struct process *c = processes; c; c = c->next) {
            if (!c->alarm_at || c->state == PROC_ZOMBIE)
                continue;
            if (c->alarm_at <= now) {
                if (c->alarm_interval) {
                    c->alarm_at += c->alarm_interval;
                    if (c->alarm_at <= now)
                        c->alarm_at += ((now - c->alarm_at) / c->alarm_interval + 1) * c->alarm_interval;
                } else {
                    c->alarm_at = 0;
                }
                signal_send(c, SIGALRM);
            }
            if (c->alarm_at && (!next || c->alarm_at < next))
                next = c->alarm_at;
        }
        uint64_t wait_ms = 0;
        if (next)
            wait_ms = next > now ? ticks_to_ms(next - now) : 1;
        if (next && !wait_ms)
            wait_ms = 1;
        wait_sleep_timeout(&alarm_wq, &proc_lock, wait_ms);
    }
}

void process_alarm_init(void)
{
    thread_create("alarmd", alarm_thread, NULL);
}

void process_thread_reaped(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    uint64_t pml4 = p->pml4_phys;
    p->pml4_phys = 0;
    bool autofree = p->autofree;
    if (autofree) {
        unregister_locked(p);
    } else if (p->parent) {
        bool exited = (p->exit_code & 0x7F) == 0;
        struct sig_src src = { exited ? CLD_EXITED : CLD_KILLED,
                               exited ? (p->exit_code >> 8) & 0xFF : p->exit_code & 0x7F, p->pid, 0, 0 };
        signal_send_info(p->parent, SIGCHLD, &src);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    if (pml4)
        vmm_destroy_address_space(pml4);
    if (autofree)
        process_free(p);
    wake_up_all(&child_exit_wq);
}

long process_getrlimit(int resource, struct user_rlimit *out)
{
    if (resource < 0 || resource >= RLIMIT_COUNT)
        return -EINVAL;
    *out = process_current()->rlimits[resource];
    return 0;
}

long process_setrlimit(int resource, const struct user_rlimit *in)
{
    if (resource < 0 || resource >= RLIMIT_COUNT || in->cur > in->max)
        return -EINVAL;
    struct process *p = process_current();
    if (in->max > p->rlimits[resource].max)
        return -EPERM;
    if (resource == RLIMIT_NOFILE && in->max > PROCESS_FD_MAX)
        return -EPERM;
    p->rlimits[resource] = *in;
    return 0;
}

int process_getppid(void)
{
    struct process *me = process_current();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int ppid = me->parent ? (int)me->parent->pid : 0;
    spin_unlock_irqrestore(&proc_lock, flags);
    return ppid;
}

int process_count(void)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int n = 0;
    for (struct process *p = processes; p; p = p->next)
        n++;
    spin_unlock_irqrestore(&proc_lock, flags);
    return n;
}

void process_dump(void)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    kprintf("  pid  ppid  estado   paginas  nome\n");
    for (struct process *p = processes; p; p = p->next)
        kprintf("  %3u  %4u  %-7s  %7lu  %s\n", p->pid, p->parent ? p->parent->pid : 0,
                p->state == PROC_ZOMBIE ? "zombie" : "running", p->pages, p->name);
    spin_unlock_irqrestore(&proc_lock, flags);
}
