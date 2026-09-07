#pragma once

#include <kernel/compiler.h>

struct limine_memmap_response;
struct limine_framebuffer;
struct limine_mp_response;

#define BOOT_MAX_MODULES 8

struct boot_module {
    const char *path;
    const char *name;
    void *address;
    uint64_t size;
};

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

    struct limine_mp_response *mp;

    uint32_t module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

extern struct boot_info boot_info;

void boot_info_init(void);

const struct boot_module *boot_find_module(const char *name);