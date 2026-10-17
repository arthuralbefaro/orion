#include <mm/uaccess.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <kernel/syscall.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/cpu.h>

#define KBUF_MAX_PAGES 16

struct ex_entry {
    uint64_t insn;
    uint64_t fixup;
};

extern const struct ex_entry __ex_table_start[], __ex_table_end[];

uint64_t uaccess_copy(void *dst, const void *src, size_t len);
int64_t uaccess_strncpy(char *dst, const char *src, size_t max);

static uint64_t fixups;

bool user_range_ok(uint64_t ptr, uint64_t len)
{
    return ptr < USER_ADDR_MAX && len <= USER_ADDR_MAX - ptr;
}

long copy_from_user(void *dst, const void *usrc, size_t len)
{
    if (!len)
        return 0;
    if (!user_range_ok((uint64_t)usrc, len))
        return -EFAULT;
    return uaccess_copy(dst, usrc, len) ? -EFAULT : 0;
}

long copy_to_user(void *udst, const void *src, size_t len)
{
    if (!len)
        return 0;
    if (!user_range_ok((uint64_t)udst, len))
        return -EFAULT;
    return uaccess_copy(udst, src, len) ? -EFAULT : 0;
}

long strncpy_from_user(char *dst, const char *usrc, size_t size)
{
    if (!size)
        return -ENAMETOOLONG;
    uint64_t u = (uint64_t)usrc;
    if (u >= USER_ADDR_MAX)
        return -EFAULT;
    size_t max = size;
    if (max > USER_ADDR_MAX - u)
        max = USER_ADDR_MAX - u;
    int64_t n = uaccess_strncpy(dst, usrc, max);
    if (n < 0) {
        dst[0] = '\0';
        return -EFAULT;
    }
    if ((size_t)n >= max) {
        dst[size - 1] = '\0';
        return max < size ? -EFAULT : -ENAMETOOLONG;
    }
    return n;
}

bool uaccess_fixup(struct interrupt_frame *f, uint64_t cr2)
{
    if (f->int_no == EXC_PAGE_FAULT && cr2 >= USER_ADDR_MAX)
        return false;
    for (const struct ex_entry *e = __ex_table_start; e < __ex_table_end; e++) {
        if (e->insn == f->rip) {
            f->rip = e->fixup;
            __atomic_fetch_add(&fixups, 1, __ATOMIC_RELAXED);
            return true;
        }
    }
    return false;
}

uint64_t uaccess_fault_count(void)
{
    return fixups;
}

void *kbuf_get(struct kbuf *b, size_t want)
{
    b->phys = 0;
    b->pages = 0;
    if (want <= KBUF_SMALL) {
        b->ptr = b->small;
        b->size = KBUF_SMALL;
        return b->ptr;
    }
    size_t pages = PAGE_ALIGN_UP(want) / PAGE_SIZE;
    if (pages > KBUF_MAX_PAGES)
        pages = KBUF_MAX_PAGES;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys && pages > 1) {
        pages = 1;
        phys = pmm_alloc_page();
    }
    if (!phys) {
        b->ptr = b->small;
        b->size = KBUF_SMALL;
        return b->ptr;
    }
    b->phys = phys;
    b->pages = pages;
    b->ptr = phys_to_virt(phys);
    b->size = pages * PAGE_SIZE;
    return b->ptr;
}

void kbuf_put(struct kbuf *b)
{
    if (b->pages)
        pmm_free_pages(b->phys, b->pages);
    b->pages = 0;
    b->ptr = NULL;
}
