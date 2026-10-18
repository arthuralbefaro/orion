/*
 * pmm.h - Physical Memory Manager (alocador de páginas físicas de 4 KiB).
 *
 * Implementação: bitmap (1 bit por página; 1 = ocupada). Simples, previsível
 * e suficiente até termos um buddy allocator.
 *
 * Todas as funções trabalham com ENDEREÇOS FÍSICOS. Use phys_to_virt() para
 * acessar o conteúdo.
 */
#pragma once

#include <kernel/compiler.h>

struct pmm_stats {
    uint64_t total_pages;   /* páginas utilizáveis conhecidas */
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t highest_addr;  /* fim da RAM utilizável mais alta */
    uint64_t bitmap_bytes;
};

void     pmm_init(void);

/* Aloca uma página. Retorna endereço físico ou 0 se acabou a memória. */
uint64_t pmm_alloc_page(void);

/* Aloca `count` páginas fisicamente contíguas. 0 em caso de falha. */
uint64_t pmm_alloc_pages(size_t count);

void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, size_t count);
uint64_t pmm_add_region(uint64_t phys, uint64_t count);

/* Contagem de referencias por pagina, usada pelo fork copy-on-write. */
void     pmm_ref_inc(uint64_t phys);
uint32_t pmm_ref_count(uint64_t phys);
uint64_t pmm_shared_pages(void);

void     pmm_get_stats(struct pmm_stats *out);

/* Imprime o mapa de memoria do bootloader. */
void     pmm_dump_memmap(void);
