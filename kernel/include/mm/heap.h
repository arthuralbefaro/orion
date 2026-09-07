#pragma once

#include <kernel/compiler.h>

#define HEAP_ALIGN 16

void heap_init(void);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

size_t kmalloc_size(const void *ptr);

struct heap_stats {
    uint64_t slab_pages;
    uint64_t slab_objs_total;
    uint64_t slab_objs_used;
    uint64_t slab_bytes_used;
    uint64_t large_count;
    uint64_t large_pages;
    uint64_t large_bytes_used;
    uint64_t alloc_calls, free_calls;
};
void heap_get_stats(struct heap_stats *out);
void heap_dump(void);

bool heap_selftest(void);