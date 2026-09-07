#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/elf.h>
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/percpu.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

static struct process *processes;
static uint32_t next_pid = 1;

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

static void copy_to_user_space(struct process *p, uint64_t virt, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys = vmm_virt_to_phys_in(p->pml4_phys, virt);
        size_t off = virt & (PAGE_SIZE - 1);
        size_t chunk = MIN(len, PAGE_SIZE - off);
        memcpy((uint8_t *)phys_to_virt(phys), s, chunk);
        virt += chunk;
        s += chunk;
        len -= chunk;
    }
}

static bool load_elf(struct process *p, const uint8_t *image, size_t size)
{
    if (size < sizeof(struct elf64_ehdr))
        return false;
    const struct elf64_ehdr *eh = (const void *)image;
    if (memcmp(eh->e_ident, ELF_MAGIC, 4) != 0 || eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB || eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
        log_error("elf", "%s: nao e um ELF64 x86_64 executavel", p->name);
        return false;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return false;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const void *)(image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_SIZE ||
            ph->p_offset + ph->p_filesz > size) {
            log_error("elf", "%s: segmento fora dos limites", p->name);
            return false;
        }
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W)
            flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X))
            flags |= PTE_NX;
        if (!map_user_range(p, ph->p_vaddr, ph->p_memsz, flags))
            return false;
        copy_to_user_space(p, ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
    }

    p->entry = eh->e_entry;
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

struct process *process_create_elf(const char *name, const void *image, size_t size)
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

    if (!load_elf(p, image, size) ||
        !map_user_range(p, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
                        PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX)) {
        vmm_destroy_address_space(p->pml4_phys);
        kfree(p);
        return NULL;
    }
    p->stack_top = USER_STACK_TOP - 8;

    uint64_t flags = irq_save();
    p->pid = next_pid++;
    p->next = processes;
    processes = p;
    irq_restore(flags);

    log_info("proc", "pid %u '%s': entry=%016lx, %lu paginas, pml4=%016lx",
             p->pid, p->name, p->entry, p->pages, p->pml4_phys);

    p->thread = thread_create_for_process(name, user_thread_entry, p, p);
    if (!p->thread) {
        process_free(p);
        return NULL;
    }
    return p;
}

struct process *process_current(void)
{
    return thread_current()->process;
}

NORETURN void process_exit(int code)
{
    struct process *p = process_current();
    ASSERT(p);
    p->exit_code = code;
    log_info("proc", "pid %u '%s' terminou com codigo %d", p->pid, p->name, code);
    thread_exit();
}

void process_free(struct process *p)
{
    struct process **pp = &processes;
    while (*pp && *pp != p)
        pp = &(*pp)->next;
    if (*pp)
        *pp = p->next;
    vmm_destroy_address_space(p->pml4_phys);
    kfree(p);
}

void process_dump(void)
{
    uint64_t flags = irq_save();
    kprintf("  pid  paginas  tid  nome\n");
    for (struct process *p = processes; p; p = p->next)
        kprintf("  %3u  %7lu  %3u  %s\n", p->pid, p->pages, p->thread ? p->thread->tid : 0, p->name);
    irq_restore(flags);
}
