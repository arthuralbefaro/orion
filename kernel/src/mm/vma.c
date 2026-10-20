#include <mm/vma.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <kernel/process.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <fs/pcache.h>
#include <arch/x86_64/cpu.h>
#include <kernel/mutex.h>

uint64_t vma_pte_flags(uint32_t flags)
{
    uint64_t f = PTE_PRESENT | PTE_USER;
    if (flags & VMA_WRITE)
        f |= PTE_WRITABLE;
    if (!(flags & VMA_EXEC))
        f |= PTE_NX;
    if (flags & VMA_NOFREE)
        f |= PTE_NOFREE;
    return f;
}

static bool vma_accessible(uint32_t flags)
{
    return (flags & (VMA_READ | VMA_WRITE | VMA_EXEC)) != 0;
}

static struct vma *vma_alloc(uint64_t start, uint64_t end, uint32_t flags, struct file *file,
                             uint64_t offset, uint64_t file_size)
{
    struct vma *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    v->start = start;
    v->end = end;
    v->flags = flags;
    v->file = file;
    v->offset = offset;
    v->file_size = file_size;
    if (file)
        file_ref(file);
    return v;
}

static void vma_free(struct vma *v)
{
    if (v->file)
        file_unref(v->file);
    kfree(v);
}

struct vma *vma_find(struct process *p, uint64_t addr)
{
    for (struct vma *v = p->vmas; v && v->start <= addr; v = v->next)
        if (addr < v->end)
            return v;
    return NULL;
}

bool vma_range_free(struct process *p, uint64_t start, uint64_t end)
{
    for (struct vma *v = p->vmas; v && v->start < end; v = v->next)
        if (v->end > start)
            return false;
    return true;
}

bool vma_add(struct process *p, uint64_t start, uint64_t end, uint32_t flags, struct file *file,
             uint64_t offset, uint64_t file_size)
{
    if (start >= end || !vma_range_free(p, start, end))
        return false;
    struct vma *n = vma_alloc(start, end, flags, file, offset, file_size);
    if (!n)
        return false;
    struct vma **pp = &p->vmas;
    while (*pp && (*pp)->start < start)
        pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    return true;
}

static uint64_t find_free_from(struct process *p, uint64_t cur, uint64_t len)
{
    for (struct vma *v = p->vmas; v; v = v->next) {
        if (v->end <= cur)
            continue;
        if (v->start >= cur + len)
            break;
        cur = v->end;
    }
    if (cur + len > USER_MMAP_END || cur + len < cur)
        return 0;
    return cur;
}

uint64_t vma_find_free(struct process *p, uint64_t len)
{
    uint64_t base = p->mmap_base ? p->mmap_base : USER_MMAP_BASE;
    uint64_t addr = find_free_from(p, base, len);
    if (!addr && base != USER_MMAP_BASE)
        addr = find_free_from(p, USER_MMAP_BASE, len);
    return addr;
}

uint64_t vma_count(struct process *p)
{
    uint64_t n = 0;
    for (struct vma *v = p->vmas; v; v = v->next)
        n++;
    return n;
}

static void drop_pages(struct process *p, uint64_t start, uint64_t end)
{
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte_in(p->pml4_phys, va);
        if (!(pte & (PTE_PRESENT | PTE_PROTNONE)))
            continue;
        vmm_unmap_page_in(p->pml4_phys, va);
        if (!(pte & PTE_NOFREE)) {
            process_defer_free(p, pte & PTE_ADDR_MASK);
            if (p->pages)
                p->pages--;
        }
    }
}

static struct vma *split(struct vma *v, uint64_t at)
{
    struct vma *n = vma_alloc(at, v->end, v->flags, v->file, v->offset + (at - v->start), v->file_size);
    if (!n)
        return NULL;
    n->next = v->next;
    v->next = n;
    v->end = at;
    return n;
}

long vma_unmap(struct process *p, uint64_t start, uint64_t end)
{
    struct vma **pp = &p->vmas;
    while (*pp) {
        struct vma *v = *pp;
        if (v->start >= end)
            break;
        if (v->end <= start) {
            pp = &v->next;
            continue;
        }
        if (v->start < start && v->end > end) {
            if (!split(v, end))
                return -ENOMEM;
        }
        if (v->start < start) {
            uint64_t old_end = v->end;
            drop_pages(p, start, old_end);
            v->end = start;
            pp = &v->next;
            continue;
        }
        if (v->end > end) {
            drop_pages(p, v->start, end);
            v->offset += end - v->start;
            v->start = end;
            break;
        }
        drop_pages(p, v->start, v->end);
        *pp = v->next;
        vma_free(v);
    }
    return 0;
}

static bool covered(struct process *p, uint64_t start, uint64_t end)
{
    uint64_t cur = start;
    for (struct vma *v = p->vmas; v && cur < end; v = v->next) {
        if (v->end <= cur)
            continue;
        if (v->start > cur)
            return false;
        cur = v->end;
    }
    return cur >= end;
}

static uint64_t reprotect_pte(uint64_t pte, uint32_t flags)
{
    uint64_t phys = pte & PTE_ADDR_MASK;
    uint64_t keep = pte & (PTE_COW | PTE_NOFREE | PTE_WRITETHRU | PTE_NOCACHE);
    if (!vma_accessible(flags))
        return phys | PTE_PROTNONE | keep;
    uint64_t nf = vma_pte_flags(flags) | keep;
    if (nf & PTE_COW)
        nf &= ~PTE_WRITABLE;
    return phys | nf;
}

long vma_protect(struct process *p, uint64_t start, uint64_t end, uint32_t prot)
{
    if (!covered(p, start, end))
        return -ENOMEM;
    for (struct vma *v = p->vmas; v && v->start < end; v = v->next) {
        if (v->end <= start)
            continue;
        if (v->start < start) {
            if (!split(v, start))
                return -ENOMEM;
            continue;
        }
        if (v->end > end && !split(v, end))
            return -ENOMEM;
        v->flags = (v->flags & ~(uint32_t)(VMA_READ | VMA_WRITE | VMA_EXEC)) | prot;
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            uint64_t pte = vmm_get_pte_in(p->pml4_phys, va);
            if (!(pte & (PTE_PRESENT | PTE_PROTNONE)))
                continue;
            vmm_set_pte_in(p->pml4_phys, va, reprotect_pte(pte, v->flags));
        }
    }
    return 0;
}

long vma_discard(struct process *p, uint64_t start, uint64_t end)
{
    for (struct vma *v = p->vmas; v && v->start < end; v = v->next) {
        if (v->end <= start || (v->flags & (VMA_SHARED | VMA_NOFREE | VMA_IMAGE)))
            continue;
        uint64_t s = v->start > start ? v->start : start;
        uint64_t e = v->end < end ? v->end : end;
        drop_pages(p, s, e);
    }
    return 0;
}

bool vma_resize(struct process *p, struct vma *v, uint64_t new_end)
{
    if (new_end > v->end) {
        if (v->next && v->next->start < new_end)
            return false;
        v->end = new_end;
        return true;
    }
    drop_pages(p, new_end, v->end);
    v->end = new_end;
    return true;
}

long vma_remap(struct process *p, uint64_t old, uint64_t old_len, uint64_t new_len, bool may_move)
{
    struct vma *v = vma_find(p, old);
    uint64_t old_end = old + old_len;
    if (!v || v->start > old || old_end > v->end)
        return -EFAULT;
    if (v->flags & (VMA_SHARED | VMA_NOFREE | VMA_FILE | VMA_IMAGE | VMA_STACK | VMA_HEAP))
        return -EINVAL;
    if (new_len == old_len)
        return (long)old;
    if (new_len < old_len) {
        long r = vma_unmap(p, old + new_len, old_end);
        return r < 0 ? r : (long)old;
    }
    uint64_t new_end = old + new_len;
    if (old_end == v->end && new_end <= USER_ADDR_MAX - PAGE_SIZE && vma_range_free(p, old_end, new_end) &&
        vma_resize(p, v, new_end))
        return (long)old;
    if (!may_move)
        return -ENOMEM;
    if (v->start < old) {
        v = split(v, old);
        if (!v)
            return -ENOMEM;
    }
    if (v->end > old_end && !split(v, old_end))
        return -ENOMEM;
    uint32_t flags = v->flags;
    uint64_t dst = vma_find_free(p, new_len);
    if (!dst || !vma_add(p, dst, dst + new_len, flags, NULL, 0, 0))
        return -ENOMEM;
    for (uint64_t off = 0; off < old_len; off += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte_in(p->pml4_phys, old + off);
        if (!(pte & (PTE_PRESENT | PTE_PROTNONE)))
            continue;
        if (!vmm_map_page_in(p->pml4_phys, dst + off, pte & PTE_ADDR_MASK, pte & ~PTE_ADDR_MASK)) {
            for (uint64_t back = 0; back < off; back += PAGE_SIZE)
                vmm_unmap_page_in(p->pml4_phys, dst + back);
            vma_unmap(p, dst, dst + new_len);
            return -ENOMEM;
        }
    }
    for (uint64_t off = 0; off < old_len; off += PAGE_SIZE)
        if (vmm_get_pte_in(p->pml4_phys, old + off) & (PTE_PRESENT | PTE_PROTNONE))
            vmm_unmap_page_in(p->pml4_phys, old + off);
    long r = vma_unmap(p, old, old_end);
    return r < 0 ? r : (long)dst;
}

bool vma_copy_all(struct process *dst, struct process *src)
{
    struct vma **tail = &dst->vmas;
    for (struct vma *v = src->vmas; v; v = v->next) {
        struct vma *n = vma_alloc(v->start, v->end, v->flags, v->file, v->offset, v->file_size);
        if (!n) {
            vma_release_all(dst);
            return false;
        }
        *tail = n;
        tail = &n->next;
    }
    return true;
}

void vma_release_all(struct process *p)
{
    struct vma *v = p->vmas;
    p->vmas = NULL;
    while (v) {
        struct vma *next = v->next;
        vma_free(v);
        v = next;
    }
}

bool vma_covers(struct process *p, uint64_t addr)
{
    if (interrupts_enabled())
        mutex_lock(&p->mm_lock);
    else if (!mutex_trylock(&p->mm_lock))
        return false;
    bool found = vma_find(p, addr) != NULL;
    mutex_unlock(&p->mm_lock);
    return found;
}

static int fault_locked(struct process *p, uint64_t addr, uint64_t err);

int vma_fault(struct process *p, uint64_t addr, uint64_t err)
{
    if (interrupts_enabled())
        mutex_lock(&p->mm_lock);
    else if (!mutex_trylock(&p->mm_lock))
        return VMA_FAULT_SEGV;
    int r = fault_locked(p, addr, err);
    mutex_unlock(&p->mm_lock);
    return r;
}

static int map_or_drop(struct process *p, uint64_t page, uint64_t phys, uint64_t flags)
{
    if (!vmm_map_page_in(p->pml4_phys, page, phys, flags)) {
        pmm_free_page(phys);
        return VMA_FAULT_NOMEM;
    }
    p->pages++;
    return VMA_FAULT_OK;
}

static int fault_locked(struct process *p, uint64_t addr, uint64_t err)
{
    struct vma *v = vma_find(p, addr);
    if (!v || !vma_accessible(v->flags) || (v->flags & VMA_NOFREE))
        return VMA_FAULT_SEGV;
    bool present = err & 1;
    bool write = err & 2;
    bool fetch = err & 16;
    if (write && !(v->flags & VMA_WRITE))
        return VMA_FAULT_SEGV;
    if (fetch && !(v->flags & VMA_EXEC))
        return VMA_FAULT_SEGV;

    uint64_t page = PAGE_ALIGN_DOWN(addr);
    uint64_t pte = vmm_get_pte_in(p->pml4_phys, page);
    if ((pte & PTE_PRESENT) && (!write || (pte & PTE_WRITABLE)) && (!fetch || !(pte & PTE_NX))) {
        invlpg(page);
        return VMA_FAULT_OK;
    }
    if (present) {
        uint64_t old = 0;
        if (!write || !(pte & PTE_COW))
            return VMA_FAULT_SEGV;
        if (!vmm_cow_fault(p->pml4_phys, page, &old) &&
            (!vfs_cache_reclaim(16) || !vmm_cow_fault(p->pml4_phys, page, &old)))
            return VMA_FAULT_NOMEM;
        if (old)
            process_defer_free(p, old);
        process_mm_tlb_sync(p);
        return VMA_FAULT_OK;
    }
    if (pte & PTE_PROTNONE)
        return VMA_FAULT_SEGV;
    if ((v->flags & VMA_FILE) && !interrupts_enabled())
        return VMA_FAULT_SEGV;

    if ((v->flags & (VMA_FILE | VMA_SHARED)) == (VMA_FILE | VMA_SHARED) && v->file->vn->mnt->type->getpage) {
        struct vnode *vn = v->file->vn;
        uint64_t phys = 0;
        int r = vn->mnt->type->getpage(vn, (v->offset + (page - v->start)) / PAGE_SIZE, &phys);
        if (r < 0)
            return r == -ENOMEM ? VMA_FAULT_NOMEM : VMA_FAULT_SEGV;
        return map_or_drop(p, page, phys, vma_pte_flags(v->flags));
    }

    if (v->flags & VMA_FILE) {
        uint64_t off = v->offset + (page - v->start);
        struct vnode *vn = v->file->vn;
        if (off < v->file_size && pcache_enabled(vn)) {
            int err = 0;
            struct pmm_stats mem;
            pmm_get_stats(&mem);
            if (mem.free_pages < 16)
                vfs_cache_reclaim(16);
            uint64_t cached = pcache_get(vn, off / PAGE_SIZE, &err);
            if (!cached)
                return err == -ENOMEM ? VMA_FAULT_NOMEM : VMA_FAULT_SEGV;
            uint64_t flags = (vma_pte_flags(v->flags) & ~PTE_WRITABLE) | PTE_COW;
            return map_or_drop(p, page, cached, flags);
        }
    }

    uint64_t phys = pmm_alloc_user_page();
    if (!phys && vfs_cache_reclaim(16))
        phys = pmm_alloc_user_page();
    if (!phys)
        return VMA_FAULT_NOMEM;
    uint8_t *mem = phys_to_virt(phys);
    memset(mem, 0, PAGE_SIZE);
    if (v->flags & VMA_FILE) {
        uint64_t off = v->offset + (page - v->start);
        if (off < v->file_size) {
            uint64_t n = v->file_size - off;
            if (n > PAGE_SIZE)
                n = PAGE_SIZE;
            if (vfs_read(v->file->vn, off, mem, n) < 0) {
                pmm_free_page(phys);
                return VMA_FAULT_SEGV;
            }
        }
    }
    return map_or_drop(p, page, phys, vma_pte_flags(v->flags));
}

bool vma_add_merge(struct process *p, uint64_t pml4, uint64_t start, uint64_t end, uint32_t flags)
{
    uint64_t cur = start;
    while (cur < end) {
        struct vma *v = p->vmas;
        while (v && v->end <= cur)
            v = v->next;
        if (!v || v->start >= end)
            return vma_add(p, cur, end, flags, NULL, 0, 0);
        if (v->start > cur) {
            if (!vma_add(p, cur, v->start, flags, NULL, 0, 0))
                return false;
            cur = v->start;
            continue;
        }
        if (v->start < cur) {
            v = split(v, cur);
            if (!v)
                return false;
        }
        uint64_t e = v->end < end ? v->end : end;
        if (v->end > e && !split(v, e))
            return false;
        v->flags |= flags;
        for (uint64_t va = cur; va < e; va += PAGE_SIZE) {
            uint64_t pte = vmm_get_pte_in(pml4, va);
            if (pte & (PTE_PRESENT | PTE_PROTNONE))
                vmm_set_pte_in(pml4, va, reprotect_pte(pte, v->flags));
        }
        cur = e;
    }
    return true;
}
