/*
 * limine_requests.c - requests do protocolo Limine.
 *
 * O bootloader varre o executável entre os marcadores START/END, encontra
 * cada request pelo seu ID e preenche o campo `response`. As variáveis são
 * `volatile` + `used` para o compilador não as descartar.
 */
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <mm/memory.h>
#include <limine.h>

#define LIMINE_REQ __attribute__((used, section(".limine_requests"))) static volatile

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

LIMINE_REQ uint64_t base_revision[] = LIMINE_BASE_REVISION(6);

LIMINE_REQ struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_firmware_type_request firmware_type_request = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0
};
LIMINE_REQ struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* ---------- ---------- */

struct boot_info boot_info;
uint64_t hhdm_offset;

void boot_info_init(void)
{
    /* Qual revisão o bootloader realmente usou? */
    if (LIMINE_BASE_REVISION_SUPPORTED(base_revision))
        boot_info.base_revision = 6;
    else if (LIMINE_LOADED_BASE_REVISION_VALID(base_revision))
        boot_info.base_revision = LIMINE_LOADED_BASE_REVISION(base_revision);
    else
        halt_forever(); /* bootloader antigo demais; não temos nem como avisar */

    if (boot_info.base_revision < 3)
        halt_forever();

    if (!hhdm_request.response || !memmap_request.response ||
        !executable_address_request.response)
        halt_forever();

    hhdm_offset = hhdm_request.response->offset;
    boot_info.hhdm_offset = hhdm_offset;
    boot_info.memmap = memmap_request.response;
    boot_info.kernel_phys_base = executable_address_request.response->physical_base;
    boot_info.kernel_virt_base = executable_address_request.response->virtual_base;

    boot_info.bootloader_name = "?";
    boot_info.bootloader_version = "?";
    if (bootloader_info_request.response) {
        boot_info.bootloader_name = bootloader_info_request.response->name;
        boot_info.bootloader_version = bootloader_info_request.response->version;
    }

    boot_info.firmware = "?";
    if (firmware_type_request.response) {
        switch (firmware_type_request.response->firmware_type) {
        case LIMINE_FIRMWARE_TYPE_X86BIOS: boot_info.firmware = "BIOS"; break;
        case LIMINE_FIRMWARE_TYPE_EFI32:   boot_info.firmware = "UEFI 32"; break;
        case LIMINE_FIRMWARE_TYPE_EFI64:   boot_info.firmware = "UEFI 64"; break;
        default: break;
        }
    }

    boot_info.framebuffer = NULL;
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0)
        boot_info.framebuffer = framebuffer_request.response->framebuffers[0];

    boot_info.rsdp = rsdp_request.response ? rsdp_request.response->address : NULL;

    boot_info.mp = mp_request.response;

    boot_info.module_count = 0;
    if (module_request.response) {
        uint64_t n = module_request.response->module_count;
        for (uint64_t i = 0; i < n && i < BOOT_MAX_MODULES; i++) {
            struct limine_file *f = module_request.response->modules[i];
            struct boot_module *m = &boot_info.modules[boot_info.module_count++];
            m->path = f->path;
            m->address = f->address;
            m->size = f->size;
            const char *name = f->path;
            for (const char *c = f->path; *c; c++)
                if (*c == '/')
                    name = c + 1;
            m->name = name;
        }
    }
}

const struct boot_module *boot_find_module(const char *name)
{
    for (uint32_t i = 0; i < boot_info.module_count; i++) {
        const char *a = boot_info.modules[i].name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*b == '\0' && (*a == '\0' || *a == '.'))
            return &boot_info.modules[i];
    }
    return NULL;
}
