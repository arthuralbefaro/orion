#pragma once

#include <kernel/compiler.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE  ((uint64_t)1 << PAGE_SHIFT)

#define PAGE_ALIGN_UP(x)   ALIGN_UP((uint64_t)(x), PAGE_SIZE)
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((uint64_t)(x), PAGE_SIZE)

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(phys + hhdm_offset);
}

static inline uint64_t virt_to_phys(const void *virt)
{
    return (uint64_t)virt - hhdm_offset;
}