#pragma once

#include <kernel/compiler.h>
#include <kernel/spinlock.h>

struct vnode;

struct pcache {
    spinlock_t lock;
    uint64_t top;
    uint8_t height;
    uint64_t npages;
    uint64_t gen;
};

void pcache_init(void);
bool pcache_enabled(const struct vnode *v);
uint64_t pcache_get(struct vnode *v, uint64_t idx, int *err);
void pcache_install(struct vnode *v, uint64_t idx, uint64_t *page);
int64_t pcache_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len);
void pcache_write(struct vnode *v, uint64_t offset, const void *buf, uint64_t len);
void pcache_truncate(struct vnode *v, uint64_t size);
void pcache_drop(struct vnode *v);
uint64_t pcache_total_pages(void);
bool pcache_over_budget(void);
uint64_t pcache_budget_pages(void);
