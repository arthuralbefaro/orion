#include <mm/pmm.h>
#include <mm/memory.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <limine.h>

static uint8_t  *bitmap;
static uint64_t  bitmap_phys;
static uint64_t  bitmap_bytes;
static uint64_t  page_count;
static uint64_t  total_pages;
static uint64_t  used_pages;
static uint64_t  highest_addr;
static uint64_t  search_hint;
static uint32_t *refcount;
static uint64_t  shared_pages;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline bool bit_test(uint64_t i)  { return (bitmap[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint64_t i)   { bitmap[i >> 3] |= (uint8_t)(1 << (i & 7)); }
static inline void bit_clear(uint64_t i) { bitmap[i >> 3] &= (uint8_t)~(1 << (i & 7)); }

static const char *memmap_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel/modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved (mapped)";
    default:                                   return "?";
    }
}

void pmm_dump_memmap(void)
{
    struct limine_memmap_response *mm = boot_info.memmap;
    kprintf("Mapa de memoria (%lu entradas):\n", mm->entry_count);
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        kprintf("  %016lx - %016lx  %8lu KiB  %s\n",
                e->base, e->base + e->length, e->length / 1024,
                memmap_type_name(e->type));
    }
}

void pmm_init(void)
{
    struct limine_memmap_response *mm = boot_info.memmap;

    highest_addr = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t end = e->base + e->length;
        if (end > highest_addr)
            highest_addr = end;
    }
    if (highest_addr == 0)
        panic("pmm: nenhuma regiao utilizavel no mapa de memoria");

    page_count   = highest_addr / PAGE_SIZE;
    bitmap_bytes = PAGE_ALIGN_UP(DIV_ROUND_UP(page_count, 8));

    bitmap = NULL;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes) {
            bitmap_phys = e->base;
            bitmap = phys_to_virt(bitmap_phys);
            break;
        }
    }
    if (!bitmap)
        panic("pmm: sem espaco para o bitmap (%lu bytes)", bitmap_bytes);

    memset(bitmap, 0xFF, bitmap_bytes);
    total_pages = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        for (uint64_t p = e->base; p < e->base + e->length; p += PAGE_SIZE) {
            bit_clear(p / PAGE_SIZE);
            total_pages++;
        }
    }

    used_pages = 0;
    for (uint64_t p = bitmap_phys; p < bitmap_phys + bitmap_bytes; p += PAGE_SIZE) {
        bit_set(p / PAGE_SIZE);
        used_pages++;
    }
    if (!bit_test(0)) {
        bit_set(0);
        used_pages++;
    }

    search_hint = bitmap_phys / PAGE_SIZE;

    uint64_t ref_bytes = ALIGN_UP(page_count * sizeof(uint32_t), PAGE_SIZE);
    uint64_t ref_phys = pmm_alloc_pages(ref_bytes / PAGE_SIZE);
    if (!ref_phys)
        panic("pmm: sem memoria para a tabela de referencias");
    refcount = phys_to_virt(ref_phys);
    memset(refcount, 0, ref_bytes);

    log_info("pmm", "RAM utilizavel: %lu MiB em %lu paginas; bitmap de %lu KiB em %016lx",
             total_pages * PAGE_SIZE / MiB(1), total_pages, bitmap_bytes / 1024, bitmap_phys);
}

static uint64_t find_free_run(uint64_t start, size_t count)
{
    uint64_t run_start = 0;
    size_t run = 0;

    for (uint64_t i = start; i < page_count; i++) {

        if (run == 0 && (i & 63) == 0 && i + 64 <= page_count) {
            uint64_t word;
            memcpy(&word, &bitmap[i >> 3], sizeof(word));
            if (word == ~0ull) {
                i += 63;
                continue;
            }
        }
        if (bit_test(i)) {
            run = 0;
            continue;
        }
        if (run == 0)
            run_start = i;
        if (++run == count)
            return run_start;
    }
    return (uint64_t)-1;
}

static uint64_t alloc_locked(size_t count)
{
    uint64_t idx = find_free_run(search_hint, count);
    if (idx == (uint64_t)-1)
        idx = find_free_run(1, count);
    if (idx == (uint64_t)-1)
        return 0;
    for (size_t i = 0; i < count; i++)
        bit_set(idx + i);
    used_pages += count;
    search_hint = idx + count;
    return idx * PAGE_SIZE;
}

uint64_t pmm_reserve_pages(void)
{
    uint64_t r = total_pages / 64;
    if (r < 256)
        r = 256;
    if (r > 4096)
        r = 4096;
    return r;
}

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0)
        return 0;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t phys = alloc_locked(count);
    spin_unlock_irqrestore(&pmm_lock, flags);
    if (!phys)
        log_error_limited("pmm", "sem memoria: pedido de %zu pagina(s)", count);
    return phys;
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

uint64_t pmm_alloc_user_page(void)
{
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t phys = 0;
    if (total_pages - used_pages > pmm_reserve_pages())
        phys = alloc_locked(1);
    spin_unlock_irqrestore(&pmm_lock, flags);
    return phys;
}

uint64_t pmm_free_count(void)
{
    return total_pages - __atomic_load_n(&used_pages, __ATOMIC_RELAXED);
}

uint64_t pmm_user_pages_available(void)
{
    uint64_t free = pmm_free_count();
    uint64_t r = pmm_reserve_pages();
    return free > r ? free - r : 0;
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    ASSERT(IS_ALIGNED(phys, PAGE_SIZE));
    uint64_t idx = phys / PAGE_SIZE;
    ASSERT(idx + count <= page_count);

    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    for (size_t i = 0; i < count; i++) {
        if (refcount && refcount[idx + i]) {
            if (--refcount[idx + i] == 0)
                shared_pages--;
            continue;
        }
        if (!bit_test(idx + i))
            panic("pmm: double free da pagina %016lx", (idx + i) * PAGE_SIZE);
        bit_clear(idx + i);
        used_pages--;
        if (idx + i < search_hint)
            search_hint = idx + i;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_add_region(uint64_t phys, uint64_t count)
{
    uint64_t added = 0;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t idx = phys / PAGE_SIZE + i;
        if (idx >= page_count || !bit_test(idx))
            continue;
        bit_clear(idx);
        total_pages++;
        added++;
        if (idx < search_hint)
            search_hint = idx;
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return added;
}

void pmm_ref_inc(uint64_t phys)
{
    uint64_t idx = phys / PAGE_SIZE;
    ASSERT(idx < page_count);
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    if (refcount[idx] == UINT32_MAX)
        panic("pmm: contador de referencias estourou na pagina %016lx", phys);
    if (refcount[idx]++ == 0)
        shared_pages++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint32_t pmm_ref_count(uint64_t phys)
{
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= page_count)
        return 1;
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint32_t n = (uint32_t)refcount[idx] + 1;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return n;
}

uint64_t pmm_shared_pages(void)
{
    return shared_pages;
}

void pmm_free_page(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

void pmm_get_stats(struct pmm_stats *out)
{
    out->total_pages  = total_pages;
    out->used_pages   = used_pages;
    out->free_pages   = total_pages - used_pages;
    out->highest_addr = highest_addr;
    out->bitmap_bytes = bitmap_bytes;
}
