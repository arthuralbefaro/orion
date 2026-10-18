#include <arch/x86_64/protect.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>

#define CPUID_7_EBX_SMEP (1u << 7)
#define CPUID_7_EBX_SMAP (1u << 20)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)

uint8_t smep_enabled;
uint8_t smap_enabled;

static bool reported;

void cpu_protect_init(void)
{
    uint32_t ebx = 0;
    if (cpuid(0, 0).eax >= 7)
        ebx = cpuid(7, 0).ebx;
    uint64_t cr4 = read_cr4();
    if (ebx & CPUID_7_EBX_SMEP) {
        smep_enabled = 1;
        cr4 |= CR4_SMEP;
    }
    if (ebx & CPUID_7_EBX_SMAP) {
        smap_enabled = 1;
        cr4 |= CR4_SMAP;
    }
    write_cr4(cr4);
    if (!reported) {
        reported = true;
        log_info("cpu", "SMEP %s, SMAP %s", smep_enabled ? "ligado" : "ausente na CPU",
                 smap_enabled ? "ligado" : "ausente na CPU");
    }
}
