#pragma once

#include <kernel/compiler.h>

struct process;
struct file;

#define VMA_READ   0x01
#define VMA_WRITE  0x02
#define VMA_EXEC   0x04
#define VMA_SHARED 0x08
#define VMA_NOFREE 0x10
#define VMA_STACK  0x20
#define VMA_HEAP   0x40
#define VMA_FILE   0x80
#define VMA_IMAGE  0x100

struct vma {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    struct file *file;
    uint64_t offset;
    uint64_t file_size;
    struct vma *next;
};

struct vma *vma_find(struct process *p, uint64_t addr);
bool vma_add(struct process *p, uint64_t start, uint64_t end, uint32_t flags, struct file *file,
             uint64_t offset, uint64_t file_size);
uint64_t vma_find_free(struct process *p, uint64_t len);
bool vma_range_free(struct process *p, uint64_t start, uint64_t end);
long vma_unmap(struct process *p, uint64_t start, uint64_t end);
long vma_protect(struct process *p, uint64_t start, uint64_t end, uint32_t prot);
long vma_discard(struct process *p, uint64_t start, uint64_t end);
bool vma_resize(struct process *p, struct vma *v, uint64_t new_end);
long vma_remap(struct process *p, uint64_t old, uint64_t old_len, uint64_t new_len, bool may_move);
bool vma_copy_all(struct process *dst, struct process *src);
void vma_release_all(struct process *p);
bool vma_fault(struct process *p, uint64_t addr, uint64_t err);
bool vma_covers(struct process *p, uint64_t addr);
uint64_t vma_pte_flags(uint32_t flags);
uint64_t vma_count(struct process *p);
bool vma_add_merge(struct process *p, uint64_t pml4, uint64_t start, uint64_t end, uint32_t flags);
