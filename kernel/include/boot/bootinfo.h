#pragma once

#include <kernel/compiler.h>

struct limine_memmap_response;
struct limine_framebuffer;

struct boot_info {
    uint64_t base_revision;
    const char *bootloader_name;
    const char *bootloader_version;
    const char *firmware;

    uint64_t hhdm_offset;
    struct limine_memmap_response *memmap;
    struct limine_framebuffer *framebuffer;

    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    void     *rsdp;
};

extern struct boot_info boot_info;

void boot_info_init(void);