#include <kernel/process.h>
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
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

extern void user_return(struct interrupt_frame *frame);

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
            ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_SIZE ||
            ph->p_filesz > ph->p_memsz || ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
            return false;
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W)
            flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X))
            flags |= PTE_NX;
        if (!map_user_range(p, pml4, ph->p_vaddr, ph->p_memsz, flags))
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

static void process_free(struct process *p)
{
    if (!p)
        return;
    kfree(p->fds);
    kfree(p);
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
    if (!p->fds) {
        process_free(p);
        return NULL;
    }
    p->fd_cap = PROCESS_FD_INITIAL;
    rlimits_default(p);
    strlcpy(p->name, name, sizeof(p->name));
    p->pml4_phys = vmm_create_address_space();
    if (!p->pml4_phys) {
        process_free(p);
        return NULL;
    }
    p->state = PROC_RUNNING;
    signal_init_process(p);
    p->mmap_next = USER_MMAP_BASE;
    strlcpy(p->cwd, "/", sizeof(p->cwd));
    return p;
}

bool process_resolve_path(struct process *p, const char *path, char *out, size_t out_size)
{
    char tmp[PROCESS_CWD_MAX * 2];
    if (path[0] == '/')
        strlcpy(tmp, path, sizeof(tmp));
    else
        ksnprintf(tmp, sizeof(tmp), "%s/%s", p->cwd, path);

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

static struct process *process_start(struct process *p, const char *name)
{
    process_register(p);
    p->thread = thread_create_for_process(name, user_thread_entry, p, p);
    if (!p->thread) {
        process_unregister(p);
        vmm_destroy_address_space(p->pml4_phys);
        process_free(p);
        return NULL;
    }
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

int process_fd_limit(struct process *p)
{
    uint64_t lim = p->rlimits[RLIMIT_NOFILE].cur;
    return lim > PROCESS_FD_MAX ? PROCESS_FD_MAX : (int)lim;
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
    struct file **n = kzalloc((size_t)cap * sizeof(struct file *));
    if (!n)
        return false;
    memcpy(n, p->fds, (size_t)p->fd_cap * sizeof(struct file *));
    kfree(p->fds);
    p->fds = n;
    p->fd_cap = cap;
    return true;
}

int process_fd_install(struct process *p, struct file *f)
{
    int limit = process_fd_limit(p);
    for (int i = 0; i < limit; i++) {
        if (i >= p->fd_cap && !process_fd_reserve(p, i))
            return -EMFILE;
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

struct file *process_fd_get(struct process *p, int fd)
{
    if (fd < 0 || fd >= p->fd_cap)
        return NULL;
    return p->fds[fd];
}

int process_fd_close(struct process *p, int fd)
{
    struct file *f = process_fd_get(p, fd);
    if (!f)
        return -EBADF;
    p->fds[fd] = NULL;
    file_unref(f);
    return 0;
}

static bool share_user_pages(struct process *child, uint64_t parent_pml4)
{
    uint64_t *pml4 = phys_to_virt(parent_pml4);
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
                    if (!(e & PTE_PRESENT)) continue;
                    uint64_t virt = (i4 << 39) | (i3 << 30) | (i2 << 21) | (i1 << 12);
                    uint64_t phys = e & PTE_ADDR_MASK;
                    uint64_t flags = e & ~PTE_ADDR_MASK;
                    if (e & PTE_NOFREE) {
                        vmm_map_page_in(child->pml4_phys, virt, phys, flags);
                        continue;
                    }
                    if (flags & PTE_WRITABLE) {
                        flags = (flags & ~PTE_WRITABLE) | PTE_COW;
                        pt[i1] = phys | flags;
                    }
                    pmm_ref_inc(phys);
                    vmm_map_page_in(child->pml4_phys, virt, phys, flags);
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
    cli();
    fpu_restore(st->fpu);
    kfree(st);
    user_return(&frame);
}

int process_fork(struct interrupt_frame *frame)
{
    struct process *parent = process_current();
    if ((uint64_t)process_count() >= parent->rlimits[RLIMIT_NPROC].cur)
        return -EAGAIN;
    struct process *child = process_alloc(parent->name);
    if (!child)
        return -ENOMEM;

    if (!share_user_pages(child, parent->pml4_phys)) {
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    child->pgid = parent->pgid;
    child->sig_blocked = parent->sig_blocked;
    memcpy(child->sig_handlers, parent->sig_handlers, sizeof(child->sig_handlers));
    strlcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->parent = parent;
    child->thread = NULL;
    memcpy(child->rlimits, parent->rlimits, sizeof(child->rlimits));
    if (parent->fd_cap > child->fd_cap) {
        struct file **n = kzalloc((size_t)parent->fd_cap * sizeof(struct file *));
        if (!n) {
            vmm_destroy_address_space(child->pml4_phys);
            process_free(child);
            return -ENOMEM;
        }
        kfree(child->fds);
        child->fds = n;
        child->fd_cap = parent->fd_cap;
    }
    for (int i = 0; i < parent->fd_cap; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i])
            file_ref(child->fds[i]);
    }

    struct fork_start *copy = kmalloc(sizeof(*copy));
    if (!copy) {
        for (int i = 0; i < child->fd_cap; i++)
            if (child->fds[i])
                file_unref(child->fds[i]);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    copy->frame = *frame;
    copy->frame.rax = 0;
    fpu_save(copy->fpu);

    process_register(child);
    child->thread = thread_create_for_process(child->name, fork_child_entry, copy, child);
    if (!child->thread) {
        process_unregister(child);
        kfree(copy);
        vmm_destroy_address_space(child->pml4_phys);
        process_free(child);
        return -ENOMEM;
    }
    return (int)child->pid;
}

int process_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *frame)
{
    struct process *p = process_current();

    char full[PROCESS_CWD_MAX];
    if (!process_resolve_path(p, path, full, sizeof(full)))
        return -ENOENT;
    uint64_t size;
    void *image = vfs_read_all(full, &size);
    if (!image)
        return -ENOENT;
    path = full;

    uint64_t old_pml4 = p->pml4_phys;
    uint64_t old_pages = p->pages, old_mmap = p->mmap_next, old_entry = p->entry;
    uint64_t old_stack = p->stack_top, old_brk_start = p->brk_start, old_brk = p->brk;
    uint64_t new_pml4 = vmm_create_address_space();
    p->pages = 0;
    p->mmap_next = USER_MMAP_BASE;
    int ret = 0;
    if (!new_pml4 ||
        !setup_image(p, new_pml4, image, size, (const char *const *)argv, (const char *const *)envp, full)) {
        if (new_pml4)
            vmm_destroy_address_space(new_pml4);
        p->pages = old_pages;
        p->mmap_next = old_mmap;
        p->entry = old_entry;
        p->stack_top = old_stack;
        p->brk_start = old_brk_start;
        p->brk = old_brk;
        ret = -ENOEXEC;
    } else {
        const char *name = path;
        for (const char *c = path; *c; c++)
            if (*c == '/')
                name = c + 1;
        strlcpy(p->name, name, sizeof(p->name));
        strlcpy(thread_current()->name, name, THREAD_NAME_MAX);
        signal_reset_on_exec(p);
        thread_current()->fs_base = 0;
        wrmsr(MSR_IA32_FS_BASE, 0);
        fpu_reset_current();
        cli();
        p->pml4_phys = new_pml4;
        write_cr3(new_pml4);
        sti();
        vmm_destroy_address_space(old_pml4);

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

int process_waitpid(int pid, int *status, int options)
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
        wait_sleep(&child_exit_wq, &proc_lock);
        if (signal_pending_current()) {
            spin_unlock_irqrestore(&proc_lock, flags);
            return -EINTR;
        }
    }
}

static NORETURN void process_terminate(int status)
{
    struct process *p = process_current();
    ASSERT(p);
    p->exit_code = status;
    p->alarm_at = 0;
    for (int i = 0; i < p->fd_cap; i++)
        if (p->fds[i]) {
            file_unref(p->fds[i]);
            p->fds[i] = NULL;
        }
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    p->state = PROC_ZOMBIE;
    orphan_children_locked(p);
    if (p->parent)
        signal_send(p->parent, SIGCHLD);
    else
        p->autofree = true;
    spin_unlock_irqrestore(&proc_lock, flags);
    thread_exit();
}

NORETURN void process_exit(int code)
{
    process_terminate((code & 0xFF) << 8);
}

NORETURN void process_exit_signal(int sig)
{
    process_terminate(sig & 0x7F);
}

int process_kill(int pid, int sig)
{
    struct process *me = process_current();
    int found = 0;
    int err = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    uint32_t group = pid == 0 ? me->pgid : (uint32_t)(-pid);
    for (struct process *c = processes; c; c = c->next) {
        bool match = pid > 0 ? (int)c->pid == pid : c->pgid == group;
        if (!match || c->state == PROC_ZOMBIE)
            continue;
        found++;
        int r = sig ? signal_send(c, sig) : 0;
        if (r < 0)
            err = r;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    if (err)
        return err;
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

long process_alarm(uint64_t seconds)
{
    struct process *p = process_current();
    uint64_t now = time_ticks();
    uint64_t left = 0;
    if (p->alarm_at > now)
        left = (p->alarm_at - now) / TIME_TICK_HZ;
    p->alarm_at = seconds ? now + seconds * TIME_TICK_HZ : 0;
    return (long)left;
}

static void alarm_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(100);
        uint64_t now = time_ticks();
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (struct process *c = processes; c; c = c->next) {
            if (c->alarm_at && c->alarm_at <= now && c->state != PROC_ZOMBIE) {
                c->alarm_at = 0;
                signal_send(c, SIGALRM);
            }
        }
        spin_unlock_irqrestore(&proc_lock, flags);
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
    if (autofree)
        unregister_locked(p);
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

int process_list(struct user_procinfo *out, int max)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int n = 0;
    for (struct process *p = processes; p && n < max; p = p->next) {
        out[n].pid = p->pid;
        out[n].ppid = p->parent ? p->parent->pid : 0;
        out[n].pgid = p->pgid;
        out[n].state = (uint32_t)p->state;
        out[n].pages = p->pages;
        strlcpy(out[n].name, p->name, PROCLIST_NAME);
        n++;
    }
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
