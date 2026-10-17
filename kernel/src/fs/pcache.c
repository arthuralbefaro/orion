#include <fs/pcache.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <lib/string.h>

#define PC_SHIFT  9
#define PC_SIZE   512u
#define PC_MASK   (PC_SIZE - 1)
#define PC_LEVELS 4

static uint64_t total_pages;
static uint64_t budget_pages;

void pcache_init(void)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    budget_pages = st.total_pages / 4;
}

uint64_t pcache_total_pages(void)
{
    return __atomic_load_n(&total_pages, __ATOMIC_RELAXED);
}

uint64_t pcache_budget_pages(void)
{
    return budget_pages;
}

bool pcache_over_budget(void)
{
    return pcache_total_pages() >= budget_pages;
}

bool pcache_enabled(const struct vnode *v)
{
    return vnode_is_reg(v) && v->mnt->type->readpage;
}

static uint64_t *table(uint64_t phys)
{
    return phys_to_virt(phys);
}

static uint64_t cover(uint8_t h)
{
    return 1ull << (PC_SHIFT * h);
}

static bool new_table(uint64_t *slot)
{
    uint64_t p = pmm_alloc_page();
    if (!p)
        return false;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    *slot = p;
    __atomic_add_fetch(&total_pages, 1, __ATOMIC_RELAXED);
    return true;
}

static uint64_t *slot_of(struct pcache *pc, uint64_t idx, bool create)
{
    if (!pc->top) {
        if (!create)
            return NULL;
        uint8_t h = 1;
        while (idx >= cover(h))
            h++;
        if (h > PC_LEVELS || !new_table(&pc->top))
            return NULL;
        pc->height = h;
    }
    while (idx >= cover(pc->height)) {
        if (!create || pc->height >= PC_LEVELS)
            return NULL;
        uint64_t grown;
        if (!new_table(&grown))
            return NULL;
        table(grown)[0] = pc->top;
        pc->top = grown;
        pc->height++;
    }
    uint64_t *slot = &pc->top;
    for (uint8_t lvl = pc->height; lvl > 0; lvl--) {
        slot = &table(*slot)[(idx >> (PC_SHIFT * (lvl - 1))) & PC_MASK];
        if (lvl > 1 && !*slot && !(create && new_table(slot)))
            return NULL;
    }
    return slot;
}

static void free_level(struct pcache *pc, uint64_t *slot, uint8_t lvl, uint64_t base, uint64_t first)
{
    if (!*slot)
        return;
    if (lvl == 0) {
        if (base >= first) {
            pmm_free_page(*slot);
            *slot = 0;
            pc->npages--;
            __atomic_sub_fetch(&total_pages, 1, __ATOMIC_RELAXED);
        }
        return;
    }
    if (base + cover(lvl) <= first)
        return;
    uint64_t *tb = table(*slot);
    for (uint64_t i = 0; i < PC_SIZE; i++)
        free_level(pc, &tb[i], (uint8_t)(lvl - 1), base + i * cover((uint8_t)(lvl - 1)), first);
    if (base >= first) {
        pmm_free_page(*slot);
        *slot = 0;
        __atomic_sub_fetch(&total_pages, 1, __ATOMIC_RELAXED);
    }
}

static void free_from(struct pcache *pc, uint64_t first)
{
    free_level(pc, &pc->top, pc->height, 0, first);
    if (!pc->top)
        pc->height = 0;
}

void pcache_install(struct vnode *v, uint64_t idx, uint64_t *page)
{
    struct pcache *pc = &v->pc;
    uint64_t fl = spin_lock_irqsave(&pc->lock);
    uint64_t *s = slot_of(pc, idx, false);
    if (s && *s) {
        uint64_t p = *s;
        pmm_ref_inc(p);
        spin_unlock_irqrestore(&pc->lock, fl);
        pmm_free_page(*page);
        *page = p;
        return;
    }
    s = pcache_over_budget() ? NULL : slot_of(pc, idx, true);
    if (s) {
        *s = *page;
        pc->npages++;
        __atomic_add_fetch(&total_pages, 1, __ATOMIC_RELAXED);
        pmm_ref_inc(*page);
    }
    spin_unlock_irqrestore(&pc->lock, fl);
}

uint64_t pcache_get(struct vnode *v, uint64_t idx, int *err)
{
    struct pcache *pc = &v->pc;
    uint64_t fl = spin_lock_irqsave(&pc->lock);
    uint64_t *s = slot_of(pc, idx, false);
    if (s && *s) {
        uint64_t p = *s;
        pmm_ref_inc(p);
        spin_unlock_irqrestore(&pc->lock, fl);
        return p;
    }
    spin_unlock_irqrestore(&pc->lock, fl);

    if (pcache_over_budget() && vfs_cache_reclaimable())
        vfs_cache_shrink();
    uint64_t page = pmm_alloc_page();
    if (!page && vfs_cache_reclaim(16))
        page = pmm_alloc_page();
    if (!page) {
        *err = -ENOMEM;
        return 0;
    }
    memset(phys_to_virt(page), 0, PAGE_SIZE);
    int r = v->mnt->type->readpage(v, idx, &page);
    if (r < 0) {
        pmm_free_page(page);
        *err = r;
        return 0;
    }
    return page;
}

int64_t pcache_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len)
{
    uint64_t size = __atomic_load_n(&v->size, __ATOMIC_ACQUIRE);
    if (offset >= size)
        return 0;
    if (len > size - offset)
        len = size - offset;
    uint8_t *out = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t within = pos % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done)
            chunk = len - done;
        int err = -EIO;
        uint64_t page = pcache_get(v, pos / PAGE_SIZE, &err);
        if (!page)
            return done ? (int64_t)done : err;
        memcpy(out + done, (uint8_t *)phys_to_virt(page) + within, chunk);
        pmm_free_page(page);
        done += chunk;
    }
    return (int64_t)done;
}

void pcache_write(struct vnode *v, uint64_t offset, const void *buf, uint64_t len)
{
    struct pcache *pc = &v->pc;
    const uint8_t *in = buf;
    uint64_t fl = spin_lock_irqsave(&pc->lock);
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t within = pos % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t *s = slot_of(pc, pos / PAGE_SIZE, false);
        if (s && *s && in)
            memcpy((uint8_t *)phys_to_virt(*s) + within, in + done, chunk);
        else if (s && *s)
            memset((uint8_t *)phys_to_virt(*s) + within, 0, chunk);
        done += chunk;
    }
    pc->gen++;
    spin_unlock_irqrestore(&pc->lock, fl);
}

void pcache_truncate(struct vnode *v, uint64_t size)
{
    struct pcache *pc = &v->pc;
    uint64_t fl = spin_lock_irqsave(&pc->lock);
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    free_from(pc, keep);
    uint64_t within = size % PAGE_SIZE;
    uint64_t *s = within ? slot_of(pc, keep - 1, false) : NULL;
    if (s && *s)
        memset((uint8_t *)phys_to_virt(*s) + within, 0, PAGE_SIZE - within);
    pc->gen++;
    spin_unlock_irqrestore(&pc->lock, fl);
}

void pcache_drop(struct vnode *v)
{
    struct pcache *pc = &v->pc;
    uint64_t fl = spin_lock_irqsave(&pc->lock);
    free_from(pc, 0);
    pc->gen++;
    spin_unlock_irqrestore(&pc->lock, fl);
}
