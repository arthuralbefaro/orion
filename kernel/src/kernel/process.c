#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/elf.h>
#include <kernel/log.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <kernel/spinlock.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <fs/vfs.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

extern void user_return(struct interrupt_frame *frame);

static struct process *processes;
static uint32_t next_pid = 1;
static spinlock_t proc_lock = SPINLOCK_INIT;

static bool map_user_range(struct process *p, uint64_t virt, uint64_t len, uint64_t flags)
{
    uint64_t start = PAGE_ALIGN_DOWN(virt);
    uint64_t end = PAGE_ALIGN_UP(virt + len);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        if (vmm_virt_to_phys_in(p->pml4_phys, v))
            continue;
        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return false;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        vmm_map_page_in(p->pml4_phys, v, phys, flags);
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

static bool load_elf(struct process *p, const uint8_t *image, size_t size, uint64_t *brk_out)
{
    if (size < sizeof(struct elf64_ehdr))
        return false;
    const struct elf64_ehdr *eh = (const void *)image;
    if (memcmp(eh->e_ident, ELF_MAGIC, 4) != 0 || eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB || eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return false;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return false;

    uint64_t highest = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_SIZE ||
            ph->p_offset + ph->p_filesz > size)
            return false;
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W)
            flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X))
            flags |= PTE_NX;
        if (!map_user_range(p, ph->p_vaddr, ph->p_memsz, flags))
            return false;
        copy_to_space(p->pml4_phys, ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
        if (ph->p_vaddr + ph->p_memsz > highest)
            highest = ph->p_vaddr + ph->p_memsz;
    }
    p->entry = eh->e_entry;
    *brk_out = PAGE_ALIGN_UP(highest);
    return true;
}

static uint64_t build_stack(struct process *p, const char *const *argv, const char *const *envp)
{
    uint64_t argc = 0, envc = 0;
    uint64_t strings_size = 0;
    while (argv && argv[argc])
        strings_size += strlen(argv[argc++]) + 1;
    while (envp && envp[envc])
        strings_size += strlen(envp[envc++]) + 1;

    uint64_t sp = USER_STACK_TOP - strings_size;
    sp &= ~0xFull;
    uint64_t str_base = sp;

    uint64_t nptrs = 1 + (argc + 1) + (envc + 1);
    if (nptrs & 1)
        sp -= 8;
    sp -= nptrs * 8;

    uint64_t *vec = kmalloc(nptrs * 8);
    vec[0] = argc;
    uint64_t cur = str_base;
    for (uint64_t i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        copy_to_space(p->pml4_phys, cur, argv[i], n);
        vec[1 + i] = cur;
        cur += n;
    }
    vec[1 + argc] = 0;
    for (uint64_t i = 0; i < envc; i++) {
        size_t n = strlen(envp[i]) + 1;
        copy_to_space(p->pml4_phys, cur, envp[i], n);
        vec[2 + argc + i] = cur;
        cur += n;
    }
    vec[2 + argc + envc] = 0;
    copy_to_space(p->pml4_phys, sp, vec, nptrs * 8);
    kfree(vec);
    return sp;
}

static bool setup_image(struct process *p, const void *image, size_t size, const char *const *argv,
                        const char *const *envp)
{
    uint64_t brk;
    if (!load_elf(p, image, size, &brk))
        return false;
    if (!map_user_range(p, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX))
        return false;
    p->brk_start = brk;
    p->brk = brk;
    p->stack_top = build_stack(p, argv, envp);
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

static struct process *process_alloc(const char *name)
{
    struct process *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    strlcpy(p->name, name, sizeof(p->name));
    p->pml4_phys = vmm_create_address_space();
    if (!p->pml4_phys) {
        kfree(p);
        return NULL;
    }
    p->state = PROC_RUNNING;
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
    p->next = processes;
    processes = p;
    spin_unlock_irqrestore(&proc_lock, flags);
}

static void process_unregister(struct process *p)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    struct process **pp = &processes;
    while (*pp && *pp != p)
        pp = &(*pp)->next;
    if (*pp)
        *pp = p->next;
    for (struct process *c = processes; c; c = c->next)
        if (c->parent == p) {
            c->parent = NULL;
            c->autofree = c->state == PROC_ZOMBIE;
        }
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
        kfree(p);
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
    if (!setup_image(p, image, size, argv, NULL)) {
        log_error("proc", "'%s': ELF invalido", name);
        vmm_destroy_address_space(p->pml4_phys);
        kfree(p);
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
    bool ok = setup_image(p, image, size, argv ? argv : def, NULL);
    kfree(image);
    if (!ok) {
        vmm_destroy_address_space(p->pml4_phys);
        kfree(p);
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

int process_fd_install(struct process *p, struct file *f)
{
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

struct file *process_fd_get(struct process *p, int fd)
{
    if (fd < 0 || fd >= PROCESS_MAX_FDS)
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

static bool copy_user_pages(struct process *child, uint64_t parent_pml4)
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
                    if (e & PTE_NOFREE) {
                        vmm_map_page_in(child->pml4_phys, virt, e & PTE_ADDR_MASK,
                                        e & ~PTE_ADDR_MASK);
                        continue;
                    }
                    uint64_t phys = pmm_alloc_page();
                    if (!phys)
                        return false;
                    memcpy(phys_to_virt(phys), phys_to_virt(e & PTE_ADDR_MASK), PAGE_SIZE);
                    vmm_map_page_in(child->pml4_phys, virt, phys, e & ~PTE_ADDR_MASK);
                    child->pages++;
                }
            }
        }
    }
    return true;
}

static void fork_child_entry(void *arg)
{
    struct interrupt_frame frame = *(struct interrupt_frame *)arg;
    kfree(arg);
    cli();
    user_return(&frame);
}

int process_fork(struct interrupt_frame *frame)
{
    struct process *parent = process_current();
    struct process *child = process_alloc(parent->name);
    if (!child)
        return -ENOMEM;

    if (!copy_user_pages(child, parent->pml4_phys)) {
        vmm_destroy_address_space(child->pml4_phys);
        kfree(child);
        return -ENOMEM;
    }
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->brk_start = parent->brk_start;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    strlcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    child->parent = parent;
    child->thread = NULL;
    for (int i = 0; i < PROCESS_MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i])
            file_ref(child->fds[i]);
    }

    struct interrupt_frame *copy = kmalloc(sizeof(*copy));
    *copy = *frame;
    copy->rax = 0;

    process_register(child);
    child->thread = thread_create_for_process(child->name, fork_child_entry, copy, child);
    if (!child->thread) {
        process_unregister(child);
        kfree(copy);
        vmm_destroy_address_space(child->pml4_phys);
        kfree(child);
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

    char **kargv = NULL, **kenvp = NULL;
    int argc = 0, envc = 0;
    while (argv && argv[argc]) argc++;
    while (envp && envp[envc]) envc++;
    kargv = kzalloc((argc + 1) * sizeof(char *));
    kenvp = kzalloc((envc + 1) * sizeof(char *));
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        kargv[i] = kmalloc(n);
        memcpy(kargv[i], argv[i], n);
    }
    for (int i = 0; i < envc; i++) {
        size_t n = strlen(envp[i]) + 1;
        kenvp[i] = kmalloc(n);
        memcpy(kenvp[i], envp[i], n);
    }

    uint64_t old_pml4 = p->pml4_phys;
    p->pml4_phys = vmm_create_address_space();
    p->pages = 0;
    p->mmap_next = USER_MMAP_BASE;
    int ret = 0;
    if (!p->pml4_phys || !setup_image(p, image, size, (const char *const *)kargv, (const char *const *)kenvp)) {
        if (p->pml4_phys)
            vmm_destroy_address_space(p->pml4_phys);
        p->pml4_phys = old_pml4;
        ret = -ENOEXEC;
    } else {
        const char *name = path;
        for (const char *c = path; *c; c++)
            if (*c == '/')
                name = c + 1;
        strlcpy(p->name, name, sizeof(p->name));
        strlcpy(thread_current()->name, name, THREAD_NAME_MAX);
        thread_current()->fs_base = 0;
        wrmsr(MSR_IA32_FS_BASE, 0);
        cli();
        write_cr3(p->pml4_phys);
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

    for (int i = 0; i < argc; i++) kfree(kargv[i]);
    for (int i = 0; i < envc; i++) kfree(kenvp[i]);
    kfree(kargv);
    kfree(kenvp);
    kfree(image);
    return ret;
}

int process_waitpid(int pid, int *status, int options)
{
    struct process *me = process_current();
    for (;;) {
        bool have_child = false;
        struct process *found = NULL;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
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
        spin_unlock_irqrestore(&proc_lock, flags);

        if (found) {
            int rpid = (int)found->pid;
            if (status)
                *status = found->exit_code;
            process_unregister(found);
            kfree(found);
            return rpid;
        }
        if (!have_child)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;
        thread_sleep_ms(5);
    }
}

NORETURN void process_exit(int code)
{
    struct process *p = process_current();
    ASSERT(p);
    p->exit_code = code;
    for (int i = 0; i < PROCESS_MAX_FDS; i++)
        if (p->fds[i]) {
            file_unref(p->fds[i]);
            p->fds[i] = NULL;
        }
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    p->state = PROC_ZOMBIE;
    if (!p->parent)
        p->autofree = true;
    spin_unlock_irqrestore(&proc_lock, flags);
    thread_exit();
}

void process_thread_reaped(struct process *p)
{
    if (p->pml4_phys) {
        vmm_destroy_address_space(p->pml4_phys);
        p->pml4_phys = 0;
    }
    if (p->autofree) {
        process_unregister(p);
        kfree(p);
    }
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
