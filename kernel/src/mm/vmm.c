#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>
#include <kernel/log.h>
#include <kernel/smp.h>

#define ENTRIES 512

static uint64_t kernel_pml4_phys;
static uint64_t mmio_next;
static bool     nx_supported;

#define EFER_NXE (1ull << 11)

static uint64_t cow_faults;
static uint64_t cow_copies;

static inline bool tlb_needs_flush(uint64_t pml4_phys)
{
    return pml4_phys == kernel_pml4_phys || pml4_phys == (read_cr3() & PTE_ADDR_MASK);
}

static inline size_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline size_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline size_t pd_index(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline size_t pt_index(uint64_t v)   { return (v >> 12) & 0x1FF; }

static uint64_t *next_level(uint64_t *table, size_t index, bool create, const char *what)
{
    uint64_t entry = table[index];

    if (entry & PTE_PRESENT) {
        if (entry & PTE_HUGE)
            panic("vmm: %s[%zu] e uma pagina grande; split nao implementado", what, index);
        return phys_to_virt(entry & PTE_ADDR_MASK);
    }

    if (!create)
        return NULL;

    uint64_t phys = pmm_alloc_page();
    if (!phys)
        panic("vmm: sem memoria para tabela de paginas (%s)", what);
    uint64_t *child = phys_to_virt(phys);
    memset(child, 0, PAGE_SIZE);

    table[index] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    return child;
}

void vmm_init(void)
{
    kernel_pml4_phys = read_cr3() & PTE_ADDR_MASK;
    mmio_next = KERNEL_MMIO_BASE;
    nx_supported = (rdmsr(MSR_IA32_EFER) & EFER_NXE) != 0;
    log_info("vmm", "PML4 do kernel em %016lx; heap em %016lx; MMIO em %016lx; NX %s",
             kernel_pml4_phys, KERNEL_HEAP_BASE, KERNEL_MMIO_BASE,
             nx_supported ? "on" : "off");
}

bool vmm_nx_supported(void)
{
    return nx_supported;
}

uint64_t vmm_kernel_pml4(void)
{
    return kernel_pml4_phys;
}

void vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    ASSERT(IS_ALIGNED(virt, PAGE_SIZE) && IS_ALIGNED(phys, PAGE_SIZE));

    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, pml4_index(virt), true, "PML4");
    uint64_t *pd   = next_level(pdpt, pdpt_index(virt), true, "PDPT");
    uint64_t *pt   = next_level(pd,   pd_index(virt),   true, "PD");

    if (!nx_supported)
        flags &= ~PTE_NX;

    pt[pt_index(virt)] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_HUGE);
    if (tlb_needs_flush(pml4_phys))
        invlpg(virt);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    vmm_map_page_in(kernel_pml4_phys, virt, phys, flags);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t pages, uint64_t flags)
{
    for (uint64_t i = 0; i < pages; i++)
        vmm_map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
}

void vmm_unmap_range(uint64_t virt, uint64_t pages, bool free_phys)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = virt + i * PAGE_SIZE;
        if (free_phys) {
            uint64_t phys = vmm_virt_to_phys(v);
            if (phys)
                pmm_free_page(PAGE_ALIGN_DOWN(phys));
        }
        vmm_unmap_page(v);
    }
    smp_flush_tlb_others();
}

void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, pml4_index(virt), false, "PML4");
    if (!pdpt) return;
    uint64_t *pd = next_level(pdpt, pdpt_index(virt), false, "PDPT");
    if (!pd) return;
    uint64_t *pt = next_level(pd, pd_index(virt), false, "PD");
    if (!pt) return;

    pt[pt_index(virt)] = 0;
    if (tlb_needs_flush(pml4_phys))
        invlpg(virt);
}

void vmm_unmap_page(uint64_t virt)
{
    vmm_unmap_page_in(kernel_pml4_phys, virt);
}

uint64_t vmm_virt_to_phys_in(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    uint64_t e = pml4[pml4_index(virt)];
    if (!(e & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(e & PTE_ADDR_MASK);

    e = pdpt[pdpt_index(virt)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) return (e & 0x000FFFFFC0000000ull) | (virt & (GiB(1) - 1));
    uint64_t *pd = phys_to_virt(e & PTE_ADDR_MASK);

    e = pd[pd_index(virt)];
    if (!(e & PTE_PRESENT)) return 0;
    if (e & PTE_HUGE) return (e & 0x000FFFFFFFE00000ull) | (virt & (MiB(2) - 1));
    uint64_t *pt = phys_to_virt(e & PTE_ADDR_MASK);

    e = pt[pt_index(virt)];
    if (!(e & PTE_PRESENT)) return 0;
    return (e & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

uint64_t vmm_virt_to_phys(uint64_t virt)
{
    return vmm_virt_to_phys_in(kernel_pml4_phys, virt);
}

uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t *pml4 = phys_to_virt(pml4_phys);

    uint64_t e = pml4[pml4_index(virt)];
    if (!(e & PTE_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(e & PTE_ADDR_MASK);

    e = pdpt[pdpt_index(virt)];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) return 0;
    uint64_t *pd = phys_to_virt(e & PTE_ADDR_MASK);

    e = pd[pd_index(virt)];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) return 0;
    uint64_t *pt = phys_to_virt(e & PTE_ADDR_MASK);

    return pt[pt_index(virt)];
}

void vmm_set_pte_in(uint64_t pml4_phys, uint64_t virt, uint64_t pte)
{
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    uint64_t *pdpt = next_level(pml4, pml4_index(virt), true, "PML4");
    uint64_t *pd   = next_level(pdpt, pdpt_index(virt), true, "PDPT");
    uint64_t *pt   = next_level(pd,   pd_index(virt),   true, "PD");
    if (!nx_supported)
        pte &= ~PTE_NX;
    pt[pt_index(virt)] = pte;
    if (tlb_needs_flush(pml4_phys))
        invlpg(virt);
}

uint64_t vmm_create_address_space(void)
{
    uint64_t phys = pmm_alloc_page();
    if (!phys)
        return 0;
    uint64_t *pml4 = phys_to_virt(phys);
    uint64_t *kpml4 = phys_to_virt(kernel_pml4_phys);
    memset(pml4, 0, PAGE_SIZE / 2);
    memcpy(pml4 + 256, kpml4 + 256, PAGE_SIZE / 2);
    return phys;
}

static void free_table_level(uint64_t *table, int level)
{
    for (size_t i = 0; i < ENTRIES; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_PRESENT) && !(level == 1 && (e & PTE_PROTNONE)))
            continue;
        if (level == 1 && (e & PTE_NOFREE))
            continue;
        uint64_t phys = e & PTE_ADDR_MASK;
        if (level > 1 && !(e & PTE_HUGE))
            free_table_level(phys_to_virt(phys), level - 1);
        pmm_free_page(phys);
    }
}

void vmm_destroy_address_space(uint64_t pml4_phys)
{
    ASSERT(pml4_phys != kernel_pml4_phys);
    if ((read_cr3() & PTE_ADDR_MASK) == pml4_phys)
        write_cr3(kernel_pml4_phys);
    uint64_t *pml4 = phys_to_virt(pml4_phys);
    for (size_t i = 0; i < 256; i++) {
        uint64_t e = pml4[i];
        if (!(e & PTE_PRESENT))
            continue;
        free_table_level(phys_to_virt(e & PTE_ADDR_MASK), 3);
        pmm_free_page(e & PTE_ADDR_MASK);
    }
    pmm_free_page(pml4_phys);
}

bool vmm_cow_fault(uint64_t pml4_phys, uint64_t virt)
{
    virt = PAGE_ALIGN_DOWN(virt);
    uint64_t pte = vmm_get_pte_in(pml4_phys, virt);
    if (!(pte & PTE_PRESENT) || !(pte & PTE_COW))
        return false;

    uint64_t phys = pte & PTE_ADDR_MASK;
    uint64_t flags = (pte & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITABLE;

    if (pmm_ref_count(phys) == 1) {
        vmm_map_page_in(pml4_phys, virt, phys, flags);
        cow_faults++;
        return true;
    }

    uint64_t fresh = pmm_alloc_page();
    if (!fresh)
        return false;
    memcpy(phys_to_virt(fresh), phys_to_virt(phys), PAGE_SIZE);
    vmm_map_page_in(pml4_phys, virt, fresh, flags);
    pmm_free_page(phys);
    cow_faults++;
    cow_copies++;
    return true;
}

void vmm_cow_stats(uint64_t *faults, uint64_t *copies)
{
    *faults = cow_faults;
    *copies = cow_copies;
}

void *vmm_map_mmio(uint64_t phys, size_t size)
{
    uint64_t offset = phys & (PAGE_SIZE - 1);
    uint64_t start  = PAGE_ALIGN_DOWN(phys);
    uint64_t end    = PAGE_ALIGN_UP(phys + size);
    uint64_t len    = end - start;

    uint64_t used = mmio_next - KERNEL_MMIO_BASE;
    if (len > KERNEL_MMIO_SIZE - used)
        panic("vmm: janela de MMIO esgotada");

    uint64_t virt = mmio_next;
    mmio_next += len;

    for (uint64_t off = 0; off < len; off += PAGE_SIZE)
        vmm_map_page(virt + off, start + off, PTE_MMIO);

    return (void *)(virt + offset);
}
