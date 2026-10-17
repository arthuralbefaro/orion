#pragma once

#include <kernel/compiler.h>

struct interrupt_frame;

#define KBUF_SMALL 256

struct kbuf {
    void *ptr;
    size_t size;
    uint64_t phys;
    size_t pages;
    uint8_t small[KBUF_SMALL];
};

bool user_range_ok(uint64_t ptr, uint64_t len);
long copy_from_user(void *dst, const void *usrc, size_t len);
long copy_to_user(void *udst, const void *src, size_t len);
long strncpy_from_user(char *dst, const char *usrc, size_t size);
bool uaccess_fixup(struct interrupt_frame *f, uint64_t cr2);
uint64_t uaccess_fault_count(void);

void *kbuf_get(struct kbuf *b, size_t want);
void kbuf_put(struct kbuf *b);
