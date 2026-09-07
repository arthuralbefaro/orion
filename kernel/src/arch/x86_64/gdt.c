#include <arch/x86_64/gdt.h>
#include <lib/string.h>
#include <kernel/log.h>

#define SEG_ACCESSED   (1ull << 40)
#define SEG_RW         (1ull << 41)
#define SEG_DC         (1ull << 42)
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

static uint64_t gdt[7] ALIGNED(16);
static struct tss tss ALIGNED(16);

static uint8_t kernel_stack[16 * 1024] ALIGNED(16);

static void set_tss_descriptor(uint64_t base, uint32_t limit)
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

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = KERNEL_CODE;
    gdt[2] = KERNEL_DATA;
    gdt[3] = USER_DATA;
    gdt[4] = USER_CODE;

    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)kernel_stack + sizeof(kernel_stack);
    tss.iopb_offset = sizeof(tss);
    set_tss_descriptor((uint64_t)&tss, sizeof(tss) - 1);

    struct gdtr gdtr = {
        .limit = sizeof(gdt) - 1,
        .base  = (uint64_t)gdt,
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

    log_info("gdt", "GDT carregada (%u entradas), TSS em %p, rsp0=%p",
             (unsigned)ARRAY_SIZE(gdt), (void *)&tss, (void *)tss.rsp0);
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
}