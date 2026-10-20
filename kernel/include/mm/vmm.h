#pragma once

#include <kernel/compiler.h>

#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_WRITETHRU (1ull << 3)
#define PTE_NOCACHE   (1ull << 4)
#define PTE_ACCESSED  (1ull << 5)
#define PTE_DIRTY     (1ull << 6)
#define PTE_HUGE      (1ull << 7)
#define PTE_GLOBAL    (1ull << 8)
#define PTE_NOFREE    (1ull << 9)
#define PTE_COW       (1ull << 10)
#define PTE_PROTNONE  (1ull << 11)
#define PTE_NX        (1ull << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

#define PTE_MMIO (PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHRU)
#define PTE_KERNEL_RW (PTE_PRESENT | PTE_WRITABLE | PTE_NX)

#define KERNEL_HEAP_BASE ((uint64_t)0xffffffffc0000000)
#define KERNEL_HEAP_SIZE MiB(512)
#define KERNEL_MMIO_BASE ((uint64_t)0xffffffffe0000000)
#define KERNEL_MMIO_SIZE MiB(512)

void vmm_init(void);
bool vmm_nx_supported(void);
uint64_t vmm_kernel_pml4(void);

uint64_t vmm_create_address_space(void);
void     vmm_destroy_address_space(uint64_t pml4_phys);
bool     vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_virt_to_phys_in(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt);
bool     vmm_set_pte_in(uint64_t pml4_phys, uint64_t virt, uint64_t pte);

bool     vmm_cow_fault(uint64_t pml4_phys, uint64_t virt, uint64_t *release);
void     vmm_cow_stats(uint64_t *faults, uint64_t *copies);

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t pages, uint64_t flags);
void vmm_unmap_range(uint64_t virt, uint64_t pages, bool free_phys);

uint64_t vmm_virt_to_phys(uint64_t virt);

void *vmm_map_mmio(uint64_t phys, size_t size);
void *vmm_map_mmio_flags(uint64_t phys, size_t size, uint64_t flags);
