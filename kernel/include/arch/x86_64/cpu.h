/*
 * cpu.h - acesso de baixo nível ao processador x86_64: portas de I/O,
 * flags, registradores de controle, MSRs e CPUID.
 *
 * Tudo aqui é inline assembly. Nada de lógica "de negócio".
 */
#pragma once

#include <kernel/compiler.h>

/* ---------- Portas de I/O (legacy ISA bus) ---------- */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/* Pequeno delay: escreve numa porta não usada (0x80). Útil entre comandos
 * para hardware lento (PIC, PIT). */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* ---------- Interrupções / flags ---------- */

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void pause(void) { __asm__ volatile("pause" ::: "memory"); }

static inline uint64_t read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    return flags;
}

#define RFLAGS_IF (1ull << 9)

static inline bool interrupts_enabled(void)
{
    return (read_rflags() & RFLAGS_IF) != 0;
}

/* Desabilita interrupções e devolve o estado anterior (para restaurar). */
static inline uint64_t irq_save(void)
{
    uint64_t flags = read_rflags();
    cli();
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & RFLAGS_IF)
        sti();
}

/* Trava a CPU para sempre (com interrupções desligadas). */
static inline NORETURN void halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

/* ---------- Registradores de controle ---------- */

#define DEFINE_READ_CR(n)                                          \
    static inline uint64_t read_cr##n(void)                        \
    {                                                              \
        uint64_t v;                                                \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v) : : "memory"); \
        return v;                                                  \
    }

#define DEFINE_WRITE_CR(n)                                         \
    static inline void write_cr##n(uint64_t v)                     \
    {                                                              \
        __asm__ volatile("mov %0, %%cr" #n : : "r"(v) : "memory"); \
    }

DEFINE_READ_CR(0)
DEFINE_READ_CR(2)
DEFINE_READ_CR(3)
DEFINE_READ_CR(4)
DEFINE_WRITE_CR(0)
DEFINE_WRITE_CR(3)
DEFINE_WRITE_CR(4)

#undef DEFINE_READ_CR
#undef DEFINE_WRITE_CR

/* ---------- MSRs ---------- */

#define MSR_IA32_APIC_BASE 0x1B
#define MSR_IA32_EFER      0xC0000080
#define MSR_IA32_STAR      0xC0000081
#define MSR_IA32_LSTAR     0xC0000082
#define MSR_IA32_FMASK     0xC0000084
#define MSR_IA32_FS_BASE   0xC0000100
#define MSR_IA32_GS_BASE   0xC0000101
#define MSR_IA32_KERNEL_GS_BASE 0xC0000102

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* ---------- CPUID ---------- */

struct cpuid_result {
    uint32_t eax, ebx, ecx, edx;
};

static inline struct cpuid_result cpuid(uint32_t leaf, uint32_t subleaf)
{
    struct cpuid_result r;
    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(subleaf));
    return r;
}

/* Bits úteis do CPUID leaf 1 */
#define CPUID_1_ECX_X2APIC (1u << 21)
#define CPUID_1_EDX_APIC   (1u << 9)
#define CPUID_1_EDX_MSR    (1u << 5)

/* ---------- Misc ---------- */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void invlpg(uintptr_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* Informações "humanas" sobre a CPU (implementadas em arch/x86_64/cpu.c) */
void cpu_get_vendor(char out[13]);
void cpu_get_brand(char out[49]);
bool cpu_has_x2apic(void);
