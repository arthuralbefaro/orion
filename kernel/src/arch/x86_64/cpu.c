/*
 * cpu.c - identificação da CPU via CPUID.
 */
#include <arch/x86_64/cpu.h>
#include <lib/string.h>

void cpu_get_vendor(char out[13])
{
    struct cpuid_result r = cpuid(0, 0);
    /* A string vem em EBX, EDX, ECX (nessa ordem esquisita mesmo). */
    memcpy(out + 0, &r.ebx, 4);
    memcpy(out + 4, &r.edx, 4);
    memcpy(out + 8, &r.ecx, 4);
    out[12] = '\0';
}

void cpu_get_brand(char out[49])
{
    struct cpuid_result max = cpuid(0x80000000, 0);
    if (max.eax < 0x80000004) {
        strlcpy(out, "(sem brand string)", 49);
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        struct cpuid_result r = cpuid(0x80000002 + i, 0);
        memcpy(out + i * 16 + 0,  &r.eax, 4);
        memcpy(out + i * 16 + 4,  &r.ebx, 4);
        memcpy(out + i * 16 + 8,  &r.ecx, 4);
        memcpy(out + i * 16 + 12, &r.edx, 4);
    }
    out[48] = '\0';

    /* Remove espaços à esquerda (Intel adora preencher com espaços). */
    char *p = out;
    while (*p == ' ')
        p++;
    if (p != out) {
        size_t len = strlen(p);
        memmove(out, p, len + 1);
    }
}

bool cpu_has_x2apic(void)
{
    struct cpuid_result r = cpuid(1, 0);
    return (r.ecx & CPUID_1_ECX_X2APIC) != 0;
}
