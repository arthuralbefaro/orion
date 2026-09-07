#pragma once

#include <kernel/compiler.h>

struct pmm_stats {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t highest_addr;
    uint64_t bitmap_bytes;
};

void     pmm_init(void);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);

void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, size_t count);
void     pmm_get_stats(struct pmm_stats *out);
void     pmm_dump_memmap(void);