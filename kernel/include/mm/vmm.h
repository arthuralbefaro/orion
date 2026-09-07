/*
 * vmm.h - gerenciador de memória virtual (tabelas de página x86_64, 4 níveis).
 *
 * Nesta versão só sabemos mapear/desmapear páginas de 4 KiB nas tabelas que
 * o bootloader nos entregou (CR3). É o suficiente para mapear MMIO (LAPIC,
 * I/O APIC, HPET...). Espaços de endereço por processo vêm depois.
 */
#pragma once

#include <kernel/compiler.h>

#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_WRITETHRU (1ull << 3)
#define PTE_NOCACHE   (1ull << 4)
#define PTE_ACCESSED  (1ull << 5)
#define PTE_DIRTY     (1ull << 6)
#define PTE_HUGE      (1ull << 7)   /* PS: página de 2 MiB (PD) ou 1 GiB (PDPT) */
#define PTE_GLOBAL    (1ull << 8)
#define PTE_NX        (1ull << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* Flags prontas para MMIO: leitura/escrita, sem cache (UC). */
#define PTE_MMIO (PTE_PRESENT | PTE_WRITABLE | PTE_NOCACHE | PTE_WRITETHRU)
/* Dados do kernel: RW, sem execução (NX é ignorado se a CPU não suportar). */
#define PTE_KERNEL_RW (PTE_PRESENT | PTE_WRITABLE | PTE_NX)

/*
 * Mapa da metade superior (PDPT 511 = últimos 1 GiB):
 *   0xffffffff80000000  kernel (imagem ELF)                    [PDPT 510]
 *   0xffffffffc0000000  heap "large" (KERNEL_HEAP, 512 MiB)     [PDPT 511]
 *   0xffffffffe0000000  janela de MMIO (KERNEL_MMIO, 512 MiB)   [PDPT 511]
 */
#define KERNEL_HEAP_BASE ((uint64_t)0xffffffffc0000000)
#define KERNEL_HEAP_SIZE MiB(512)
#define KERNEL_MMIO_BASE ((uint64_t)0xffffffffe0000000)
#define KERNEL_MMIO_SIZE MiB(512)

void vmm_init(void);
bool vmm_nx_supported(void);

/* Mapeia uma página de 4 KiB. Aloca tabelas intermediárias se preciso. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);

/* Versões para faixas contíguas. Em unmap, free_phys=true devolve as
 * páginas físicas ao PMM. */
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t pages, uint64_t flags);
void vmm_unmap_range(uint64_t virt, uint64_t pages, bool free_phys);

/* Traduz um endereço virtual (0 se não mapeado). */
uint64_t vmm_virt_to_phys(uint64_t virt);

/* Reserva `size` bytes da janela de MMIO e mapeia `phys` lá. */
void *vmm_map_mmio(uint64_t phys, size_t size);