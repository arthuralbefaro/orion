#include <arch/x86_64/gdt.h>
#include <arch/x86_64/percpu.h>
#include <lib/string.h>
#include <kernel/log.h>

#define SEG_RW         (1ull << 41)
#define SEG_EXEC       (1ull << 43)
#define SEG_NONSYS     (1ull << 44)
#define SEG_DPL(n)     ((uint64_t)(n) << 45)
#define SEG_PRESENT    (1ull << 47)
#define SEG_LONG       (1ull << 53)
#define SEG_DB         (1ull << 54)
#define SEG_GRAN4K     (1ull << 55)
#define SEG_LIMIT_MAX  (0xFFFFull | (0xFull << 48))

#define KERNEL_CODE (SEG_LIMIT_MAX | SEG_RW | SEG_EXEC | SEG_NONSYS | SEG_DPL(0) | SEG_PRESENT | SEG_LONG | SEG_GRAN4K)
#define KERNEL_DATA (SEG_LIMIT_MAX | SEG_RW | SEG_NONSYS | SEG_DPL(0) | SEG_PRESENT | SEG_DB | SEG_GRAN4K)
#define USER_CODE   (SEG_LIMIT_MAX | SEG_RW | SEG_EXEC | SEG_NONSYS | SEG_DPL(3) | SEG_PRESENT | SEG_LONG | SEG_GRAN4K)
#define USER_DATA   (SEG_LIMIT_MAX | SEG_RW | SEG_NONSYS | SEG_DPL(3) | SEG_PRESENT | SEG_DB | SEG_GRAN4K)

struct gdtr {
    uint16_t limit;
    uint64_t base;
} PACKED;

struct cpu_gdt {
    uint64_t gdt[7];
    struct tss tss;
} ALIGNED(16);

static struct cpu_gdt cpu_gdts[MAX_CPUS];
static uint8_t bsp_stack[16 * 1024] ALIGNED(16);

static void set_tss_descriptor(uint64_t *gdt, uint64_t base, uint32_t limit)
{
    uint64_t lo = 0, hi = 0;
    lo |= (uint64_t)(limit & 0xFFFF);
    lo |= (uint64_t)(base & 0xFFFFFF) << 16;
    lo |= (uint64_t)0x9 << 40;
    lo |= SEG_PRESENT;
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;
    hi  = (base >> 32) & 0xFFFFFFFF;
    gdt[5] = lo;
    gdt[6] = hi;
}

void gdt_init_cpu(uint32_t cpu)
{
    struct cpu_gdt *g = &cpu_gdts[cpu];
    g->gdt[0] = 0;
    g->gdt[1] = KERNEL_CODE;
    g->gdt[2] = KERNEL_DATA;
    g->gdt[3] = USER_DATA;
    g->gdt[4] = USER_CODE;

    memset(&g->tss, 0, sizeof(g->tss));
    g->tss.rsp0 = cpu == 0 ? (uint64_t)bsp_stack + sizeof(bsp_stack) : 0;
    g->tss.iopb_offset = sizeof(g->tss);
    set_tss_descriptor(g->gdt, (uint64_t)&g->tss, sizeof(g->tss) - 1);

    struct gdtr gdtr = {
        .limit = sizeof(g->gdt) - 1,
        .base  = (uint64_t)g->gdt,
    };

    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorw %%ax, %%ax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :
        : "m"(gdtr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"((uint16_t)GDT_TSS) : "memory");
}

void gdt_init(void)
{
    gdt_init_cpu(0);
    log_info("gdt", "GDT/TSS por CPU (ate %u CPUs), cpu0 carregada", MAX_CPUS);
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    cpu_gdts[percpu_cpu_id()].tss.rsp0 = rsp0;
}
