#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/irqctx.h>

#define SLAB_MAGIC   0x534C4142u
#define SLAB_MAX_OBJ 2048

struct slab {
    uint32_t     magic;
    uint16_t     obj_size;
    uint16_t     free_count;
    uint16_t     total;
    uint16_t     cache_idx;
    void        *free_list;
    struct slab *next;
    struct slab *prev;
};

#define SLAB_HDR_SIZE ALIGN_UP(sizeof(struct slab), HEAP_ALIGN)

struct slab_cache {
    uint16_t     obj_size;
    struct slab *partial;
    uint64_t     slab_count;
    uint64_t     objs_total;
    uint64_t     objs_used;
};

static const uint16_t cache_sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define CACHE_COUNT ARRAY_SIZE(cache_sizes)
static struct slab_cache caches[CACHE_COUNT];

static spinlock_t heap_lock = SPINLOCK_INIT;
static uint64_t alloc_calls, free_calls;

static int cache_for_size(size_t size)
{
    for (size_t i = 0; i < CACHE_COUNT; i++)
        if (size <= cache_sizes[i])
            return (int)i;
    return -1;
}

static void partial_insert(struct slab_cache *c, struct slab *s)
{
    s->prev = NULL;
    s->next = c->partial;
    if (c->partial)
        c->partial->prev = s;
    c->partial = s;
}

static void partial_remove(struct slab_cache *c, struct slab *s)
{
    if (s->prev) s->prev->next = s->next;
    else         c->partial = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static struct slab *slab_create(struct slab_cache *c, int idx)
{
    uint64_t phys = pmm_alloc_page();
    if (!phys)
        return NULL;

    struct slab *s = phys_to_virt(phys);
    s->magic      = SLAB_MAGIC;
    s->obj_size   = c->obj_size;
    s->cache_idx  = (uint16_t)idx;
    s->total      = (uint16_t)((PAGE_SIZE - SLAB_HDR_SIZE) / c->obj_size);
    s->free_count = s->total;
    s->next = s->prev = NULL;

    uint8_t *base = (uint8_t *)s + SLAB_HDR_SIZE;
    s->free_list = base;
    for (uint16_t i = 0; i < s->total; i++) {
        void **obj = (void **)(base + (size_t)i * c->obj_size);
        *obj = (i + 1 < s->total) ? (void *)(base + (size_t)(i + 1) * c->obj_size) : NULL;
    }

    c->slab_count++;
    c->objs_total += s->total;
    partial_insert(c, s);
    return s;
}

static void slab_destroy(struct slab_cache *c, struct slab *s)
{
    partial_remove(c, s);
    c->slab_count--;
    c->objs_total -= s->total;
    s->magic = 0;
    pmm_free_page(virt_to_phys(s));
}

static void *slab_alloc(int idx)
{
    struct slab_cache *c = &caches[idx];
    struct slab *s = c->partial;
    if (!s) {
        s = slab_create(c, idx);
        if (!s)
            return NULL;
    }

    void **obj = s->free_list;
    s->free_list = *obj;
    s->free_count--;
    c->objs_used++;

    if (s->free_count == 0)
        partial_remove(c, s);

    return obj;
}

static void slab_free(struct slab *s, void *ptr)
{
    if (s->magic != SLAB_MAGIC)
        panic("kfree: %p nao pertence a um slab valido", ptr);

    uint8_t *base = (uint8_t *)s + SLAB_HDR_SIZE;
    size_t off = (size_t)((uint8_t *)ptr - base);
    if (off % s->obj_size != 0 || off / s->obj_size >= s->total)
        panic("kfree: %p esta no meio de um objeto (slab %u)", ptr, s->obj_size);

    struct slab_cache *c = &caches[s->cache_idx];

    uint16_t seen = 0;
    for (void **p = s->free_list; p; p = *p) {
        if ((void *)p == ptr)
            panic("kfree: double free de %p (slab %u)", ptr, s->obj_size);
        if (++seen > s->total)
            panic("kfree: lista livre do slab %p (%u bytes) corrompida", (void *)s, s->obj_size);
    }

    *(void **)ptr = s->free_list;
    s->free_list = ptr;
    s->free_count++;
    c->objs_used--;

    if (s->free_count == 1)
        partial_insert(c, s);

    if (s->free_count == s->total && (c->partial != s || s->next))
        slab_destroy(c, s);
}

static inline bool in_hhdm(uintptr_t a)
{
    return a >= hhdm_offset && a < hhdm_offset + GiB(64);
}

#define LARGE_MAGIC  0x4C415247u
#define LARGE_DEAD   0x44454144u
#define LARGE_HDR    64

struct large_hdr {
    uint32_t magic;
    uint32_t pad;
    uint64_t pages;
    uint64_t size;
    struct large_hdr *next;
};

struct vrange {
    uint64_t base;
    uint64_t pages;
    struct vrange *next;
};

static struct vrange *free_ranges;
static uint64_t large_count, large_pages, large_bytes;
static struct large_hdr *reclaim_list;
static uint64_t reclaim_pending, reclaim_rounds, reclaimed_pages;
static bool deferred;

static struct vrange *vrange_node(void)
{
    struct vrange *r = slab_alloc(cache_for_size(sizeof(struct vrange)));
    if (!r)
        panic("heap: sem memoria para no de faixa virtual");
    return r;
}

static void vrange_node_free(struct vrange *r)
{
    slab_free((struct slab *)PAGE_ALIGN_DOWN((uint64_t)r), r);
}

static uint64_t vrange_alloc(uint64_t pages)
{
    for (struct vrange **pp = &free_ranges; *pp; pp = &(*pp)->next) {
        struct vrange *r = *pp;
        if (r->pages < pages)
            continue;
        uint64_t base = r->base;
        r->base  += pages * PAGE_SIZE;
        r->pages -= pages;
        if (r->pages == 0) {
            *pp = r->next;
            vrange_node_free(r);
        }
        return base;
    }
    return 0;
}

static void vrange_free(uint64_t base, uint64_t pages)
{
    struct vrange **pp = &free_ranges;
    while (*pp && (*pp)->base < base)
        pp = &(*pp)->next;

    struct vrange *next = *pp;

    if (next && base + pages * PAGE_SIZE == next->base) {
        next->base = base;
        next->pages += pages;
        struct vrange *prev = free_ranges;
        if (prev != next) {
            while (prev->next != next) prev = prev->next;
            if (prev->base + prev->pages * PAGE_SIZE == next->base) {
                prev->pages += next->pages;
                prev->next = next->next;
                vrange_node_free(next);
            }
        }
        return;
    }

    if (pp != &free_ranges) {
        struct vrange *prev = free_ranges;
        while (prev->next != next) prev = prev->next;
        if (prev->base + prev->pages * PAGE_SIZE == base) {
            prev->pages += pages;
            return;
        }
    }

    struct vrange *r = vrange_node();
    r->base = base;
    r->pages = pages;
    r->next = next;
    *pp = r;
}

static void unmap_now(uint64_t base, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = base + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(v);
        vmm_unmap_page(v);
        if (phys)
            pmm_free_page(PAGE_ALIGN_DOWN(phys));
    }
}

static void *large_alloc(size_t size)
{
    uint64_t pages = DIV_ROUND_UP(size + LARGE_HDR, PAGE_SIZE);
    uint64_t guard = vrange_alloc(pages + 1);
    if (!guard) {
        log_error("heap", "regiao virtual do heap esgotada (%lu paginas)", pages);
        return NULL;
    }
    uint64_t base = guard + PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            unmap_now(base, i);
            smp_flush_tlb_others();
            vrange_free(guard, pages + 1);
            log_error("heap", "sem memoria fisica para %lu paginas", pages);
            return NULL;
        }
        vmm_map_page(base + i * PAGE_SIZE, phys, PTE_KERNEL_RW);
    }

    struct large_hdr *h = (struct large_hdr *)base;
    h->magic = LARGE_MAGIC;
    h->pages = pages;
    h->size  = size;
    h->next  = NULL;

    large_count++;
    large_pages += pages;
    large_bytes += size;
    return (uint8_t *)base + LARGE_HDR;
}

static void large_free(void *ptr)
{
    struct large_hdr *h = (struct large_hdr *)((uint8_t *)ptr - LARGE_HDR);
    if (!IS_ALIGNED((uint64_t)h, PAGE_SIZE))
        panic("kfree: %p nao e uma alocacao grande valida", ptr);
    if (h->magic == LARGE_DEAD)
        panic("kfree: double free de %p (alocacao grande)", ptr);
    if (h->magic != LARGE_MAGIC)
        panic("kfree: %p nao e uma alocacao grande valida", ptr);

    uint64_t base = (uint64_t)h;
    uint64_t pages = h->pages;
    large_count--;
    large_pages -= pages;
    large_bytes -= h->size;

    if (deferred) {
        h->magic = LARGE_DEAD;
        h->next = reclaim_list;
        reclaim_list = h;
        reclaim_pending += pages;
        return;
    }

    h->magic = 0;
    unmap_now(base, pages);
    smp_flush_tlb_others();
    vrange_free(base - PAGE_SIZE, pages + 1);
}

static inline bool in_heap_region(uintptr_t a)
{
    return a >= KERNEL_HEAP_BASE && a < KERNEL_HEAP_BASE + KERNEL_HEAP_SIZE;
}

#define RECLAIM_BATCH  64
#define RECLAIM_RANGES 32

void heap_reclaim(void)
{
    if (!deferred)
        return;
    ASSERT(interrupts_enabled());

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    struct large_hdr *list = reclaim_list;
    reclaim_list = NULL;
    reclaim_pending = 0;
    spin_unlock_irqrestore(&heap_lock, flags);

    while (list) {
        uint64_t bases[RECLAIM_RANGES], counts[RECLAIM_RANGES];
        int nr = 0;
        while (list && nr < RECLAIM_RANGES) {
            bases[nr] = (uint64_t)list;
            counts[nr] = list->pages;
            list = list->next;
            nr++;
        }

        uint64_t batch[RECLAIM_BATCH];
        int nb = 0;
        uint64_t total = 0;
        for (int r = 0; r < nr; r++) {
            for (uint64_t i = 0; i < counts[r]; i++) {
                uint64_t v = bases[r] + i * PAGE_SIZE;
                uint64_t phys = vmm_virt_to_phys(v);
                vmm_unmap_page(v);
                if (phys)
                    batch[nb++] = PAGE_ALIGN_DOWN(phys);
                if (nb == RECLAIM_BATCH) {
                    smp_tlb_shootdown_sync();
                    for (int k = 0; k < nb; k++)
                        pmm_free_page(batch[k]);
                    total += (uint64_t)nb;
                    nb = 0;
                }
            }
        }
        smp_tlb_shootdown_sync();
        for (int k = 0; k < nb; k++)
            pmm_free_page(batch[k]);
        total += (uint64_t)nb;

        flags = spin_lock_irqsave(&heap_lock);
        for (int r = 0; r < nr; r++)
            vrange_free(bases[r] - PAGE_SIZE, counts[r] + 1);
        reclaim_rounds++;
        reclaimed_pages += total;
        spin_unlock_irqrestore(&heap_lock, flags);
    }
}

static void reclaim_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(10);
        if (reclaim_list)
            heap_reclaim();
    }
}

void heap_start_reclaimer(void)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    deferred = true;
    spin_unlock_irqrestore(&heap_lock, flags);
    thread_create("kreclaim", reclaim_thread, NULL);
    log_info("heap", "liberacao de paginas grandes adiada, com TLB shootdown sincrono");
}

void heap_init(void)
{
    for (size_t i = 0; i < CACHE_COUNT; i++) {
        caches[i].obj_size = cache_sizes[i];
        caches[i].partial = NULL;
        caches[i].slab_count = caches[i].objs_total = caches[i].objs_used = 0;
    }

    free_ranges = vrange_node();
    free_ranges->base = KERNEL_HEAP_BASE;
    free_ranges->pages = KERNEL_HEAP_SIZE / PAGE_SIZE;
    free_ranges->next = NULL;

    log_info("heap", "slabs de %u a %u bytes; regiao 'large' em %016lx (%lu MiB), pagina de guarda em cada bloco",
             cache_sizes[0], cache_sizes[CACHE_COUNT - 1],
             KERNEL_HEAP_BASE, KERNEL_HEAP_SIZE / MiB(1));
}

static void *alloc_raw(size_t size)
{
    if (size == 0)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&heap_lock);
    alloc_calls++;
    void *p;
    int idx = cache_for_size(size);
    if (idx >= 0)
        p = slab_alloc(idx);
    else
        p = large_alloc(size);
    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}

void *kmalloc(size_t size)
{
    MIGHT_SLEEP("kmalloc");
    return alloc_raw(size);
}

static void *zalloc_raw(size_t size)
{
    void *p = alloc_raw(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kzalloc(size_t size)
{
    MIGHT_SLEEP("kzalloc");
    return zalloc_raw(size);
}

void *kcalloc(size_t n, size_t size)
{
    MIGHT_SLEEP("kcalloc");
    if (n && size > SIZE_MAX / n)
        return NULL;
    return zalloc_raw(n * size);
}

size_t kmalloc_size(const void *ptr)
{
    uintptr_t a = (uintptr_t)ptr;
    if (in_hhdm(a)) {
        struct slab *s = (struct slab *)PAGE_ALIGN_DOWN(a);
        return s->obj_size;
    }
    if (in_heap_region(a)) {
        struct large_hdr *h = (struct large_hdr *)(a - LARGE_HDR);
        return h->pages * PAGE_SIZE - LARGE_HDR;
    }
    return 0;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uintptr_t a = (uintptr_t)ptr;
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    free_calls++;
    if (in_hhdm(a))
        slab_free((struct slab *)PAGE_ALIGN_DOWN(a), ptr);
    else if (in_heap_region(a))
        large_free(ptr);
    else
        panic("kfree: ponteiro %p nao veio do kmalloc", ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}

void *krealloc(void *ptr, size_t size)
{
    MIGHT_SLEEP("krealloc");
    if (!ptr)
        return alloc_raw(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    size_t old = kmalloc_size(ptr);
    if (size <= old && (old <= SLAB_MAX_OBJ || size > old / 2))
        return ptr;

    void *np = alloc_raw(size);
    if (!np)
        return NULL;
    memcpy(np, ptr, MIN(old, size));
    kfree(ptr);
    return np;
}

bool heap_in_guard(const void *block, uint64_t addr)
{
    uintptr_t a = (uintptr_t)block;
    if (!in_heap_region(a))
        return false;
    uint64_t guard_top = PAGE_ALIGN_DOWN(a);
    return addr >= guard_top - PAGE_SIZE && addr < a;
}

void heap_get_stats(struct heap_stats *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    for (size_t i = 0; i < CACHE_COUNT; i++) {
        out->slab_pages      += caches[i].slab_count;
        out->slab_objs_total += caches[i].objs_total;
        out->slab_objs_used  += caches[i].objs_used;
        out->slab_bytes_used += caches[i].objs_used * caches[i].obj_size;
    }
    out->large_count      = large_count;
    out->large_pages      = large_pages;
    out->large_bytes_used = large_bytes;
    out->alloc_calls      = alloc_calls;
    out->free_calls       = free_calls;
    out->reclaim_pending  = reclaim_pending;
    spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_dump(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    kprintf("  tamanho  slabs  objetos  em uso\n");
    for (size_t i = 0; i < CACHE_COUNT; i++)
        kprintf("  %7u  %5lu  %7lu  %6lu\n", caches[i].obj_size, caches[i].slab_count,
                caches[i].objs_total, caches[i].objs_used);
    kprintf("  large: %lu alocacoes, %lu paginas, %lu bytes pedidos\n",
            hs.large_count, hs.large_pages, hs.large_bytes_used);
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    uint64_t free_pages = 0, free_blocks = 0;
    for (struct vrange *r = free_ranges; r; r = r->next) {
        free_pages += r->pages;
        free_blocks++;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    kprintf("  regiao virtual livre: %lu MiB em %lu bloco(s)\n",
            free_pages * PAGE_SIZE / MiB(1), free_blocks);
    kprintf("  liberacao adiada: %lu pagina(s) pendente(s), %lu rodada(s), %lu pagina(s) devolvida(s)\n",
            hs.reclaim_pending, reclaim_rounds, reclaimed_pages);
    kprintf("  chamadas: %lu kmalloc, %lu kfree\n", hs.alloc_calls, hs.free_calls);
}

static uint32_t rng_state = 0x12345678;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void settle(void)
{
    if (deferred && interrupts_enabled())
        heap_reclaim();
}

bool heap_selftest(void)
{
    struct pmm_stats pm0, pm1;
    struct heap_stats hs0, hs1;
    settle();
    pmm_get_stats(&pm0);
    heap_get_stats(&hs0);

    enum { N = 400 };
    static uint8_t *ptrs[N];
    static uint32_t sizes[N];
    bool ok = true;

    for (int i = 0; i < N; i++) {
        sizes[i] = 1 + rng() % 12000;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) { kprintf("  FALHA: kmalloc(%u) retornou NULL\n", sizes[i]); return false; }
        if ((uintptr_t)ptrs[i] % HEAP_ALIGN) { kprintf("  FALHA: %p desalinhado\n", (void *)ptrs[i]); ok = false; }
        memset(ptrs[i], (int)(i & 0xFF), sizes[i]);
    }

    for (int i = 1; i < N; i += 2) {
        kfree(ptrs[i]);
        sizes[i] = 1 + rng() % 3000;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) { kprintf("  FALHA na realocacao %d\n", i); return false; }
        memset(ptrs[i], (int)(i & 0xFF), sizes[i]);
    }

    for (int i = 0; i < N; i++) {
        for (uint32_t j = 0; j < sizes[i]; j++) {
            if (ptrs[i][j] != (uint8_t)(i & 0xFF)) {
                kprintf("  FALHA: bloco %d corrompido no byte %u\n", i, j);
                ok = false;
                break;
            }
        }
    }

    uint8_t *r = kmalloc(100);
    memset(r, 0xAB, 100);
    r = krealloc(r, 5000);
    for (int i = 0; i < 100; i++) if (r[i] != 0xAB) { ok = false; kprintf("  FALHA: krealloc perdeu dados\n"); break; }
    kfree(r);

    for (int i = 0; i < N; i++)
        kfree(ptrs[i]);

    settle();
    pmm_get_stats(&pm1);
    heap_get_stats(&hs1);
    if (hs1.slab_objs_used != hs0.slab_objs_used || hs1.large_count != hs0.large_count) {
        kprintf("  FALHA: vazamento no heap (objs %lu->%lu, large %lu->%lu)\n",
                hs0.slab_objs_used, hs1.slab_objs_used, hs0.large_count, hs1.large_count);
        ok = false;
    }
    if (pm1.used_pages > pm0.used_pages + CACHE_COUNT) {
        kprintf("  FALHA: vazamento de paginas fisicas (%lu -> %lu)\n", pm0.used_pages, pm1.used_pages);
        ok = false;
    }

    kprintf("  %d blocos, %lu kmalloc/%lu kfree, paginas fisicas em uso %lu -> %lu: %s\n",
            N, hs1.alloc_calls - hs0.alloc_calls, hs1.free_calls - hs0.free_calls,
            pm0.used_pages, pm1.used_pages, ok ? "OK" : "FALHOU");
    return ok;
}
