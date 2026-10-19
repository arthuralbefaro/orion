#include <drivers/fwcfg.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>

#define FWCFG_SELECTOR 0x510
#define FWCFG_DATA     0x511
#define FWCFG_SIGNATURE 0x0000
#define FWCFG_FILE_DIR  0x0019
#define FWCFG_NAME_MAX  56
#define CPUID_1_ECX_HYPERVISOR (1u << 31)

static void read_bytes(void *out, uint32_t len)
{
    uint8_t *b = out;
    for (uint32_t i = 0; i < len; i++)
        b[i] = inb(FWCFG_DATA);
}

static uint32_t be32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

bool fwcfg_has_file(const char *name)
{
    if (!(cpuid(1, 0).ecx & CPUID_1_ECX_HYPERVISOR))
        return false;
    uint8_t sig[4];
    outw(FWCFG_SELECTOR, FWCFG_SIGNATURE);
    read_bytes(sig, sizeof(sig));
    if (memcmp(sig, "QEMU", 4) != 0)
        return false;
    uint8_t raw[4];
    outw(FWCFG_SELECTOR, FWCFG_FILE_DIR);
    read_bytes(raw, sizeof(raw));
    uint32_t count = be32(raw);
    if (count > 4096)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t head[8];
        char file[FWCFG_NAME_MAX];
        read_bytes(head, sizeof(head));
        read_bytes(file, sizeof(file));
        file[FWCFG_NAME_MAX - 1] = 0;
        if (strcmp(file, name) == 0)
            return true;
    }
    return false;
}
