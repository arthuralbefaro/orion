#pragma once

#include <kernel/compiler.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

#define GDT_RPL_USER    3

#define GDT_IST_COUNT   2
#define GDT_IST_SIZE    8192
#define GDT_IST_DOUBLE_FAULT 1
#define GDT_IST_NMI     2

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} PACKED;

void gdt_init(void);
void gdt_init_cpu(uint32_t cpu);
void gdt_set_kernel_stack(uint64_t rsp0);
bool gdt_alloc_ist(uint32_t cpu);
bool gdt_in_ist(uint64_t addr);
