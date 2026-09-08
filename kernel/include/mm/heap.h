/*
 * heap.h - alocador de memória do kernel (kmalloc/kfree).
 *
 * Duas estratégias, escolhidas pelo tamanho:
 *   - até 2048 bytes: slab allocator. Cada slab é UMA página física acessada
 *     pelo HHDM, fatiada em objetos do mesmo tamanho (16, 32, ..., 2048).
 *   - acima disso: "large": páginas físicas avulsas mapeadas numa região
 *     virtual contígua do kernel (KERNEL_HEAP_BASE), com cabeçalho.
 *
 * kfree() descobre qual caso é pelo endereço: slabs vivem no HHDM, larges
 * vivem na região do heap.
 */
#pragma once

#include <kernel/compiler.h>

#define HEAP_ALIGN 16   /* alinhamento mínimo garantido */

void  heap_init(void);

void *kmalloc(size_t size);
void *kzalloc(size_t size);            /* kmalloc + zera */
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

/* Tamanho utilizável do bloco apontado por ptr (>= size pedido). */
size_t kmalloc_size(const void *ptr);

struct heap_stats {
    uint64_t slab_pages;       /* páginas em uso pelos slabs */
    uint64_t slab_objs_total;  /* objetos alocáveis nos slabs existentes */
    uint64_t slab_objs_used;
    uint64_t slab_bytes_used;  /* soma dos obj_size em uso */
    uint64_t large_count;      /* alocações grandes vivas */
    uint64_t large_pages;      /* páginas em uso por elas */
    uint64_t large_bytes_used; /* soma dos tamanhos pedidos */
    uint64_t alloc_calls, free_calls;
};
void heap_get_stats(struct heap_stats *out);
void heap_dump(void);   /* imprime uma tabela por cache (comando "heap") */

/* Auto-teste; retorna true se passou. */
bool heap_selftest(void);
