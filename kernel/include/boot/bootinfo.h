/*
 * bootinfo.h - o que o bootloader nos entregou, já digerido.
 *
 * O resto do kernel deve consumir `boot_info` em vez de tocar nos requests
 * do Limine diretamente; assim trocar/atualizar o protocolo fica localizado
 * em boot/limine_requests.c.
 */
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
    uint64_t base_revision;         /* revisão do protocolo efetivamente usada */
    const char *bootloader_name;
    const char *bootloader_version;
    const char *firmware;           /* "UEFI 64", "BIOS"... */
    const char *kernel_path;
    const char *kernel_media;
    uint32_t kernel_partition;

    uint64_t hhdm_offset;
    struct limine_memmap_response *memmap;
    struct limine_framebuffer *framebuffer;  /* NULL se não houver */

    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
    void    *rsdp;                  /* tabela raiz da ACPI (para o futuro) */

    struct limine_mp_response *mp;

    uint32_t module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

const struct boot_module *boot_find_module(const char *name);

extern struct boot_info boot_info;

/* Lê as respostas do bootloader. Trava a CPU se algo essencial faltar. */
void boot_info_init(void);
