#include <arch/x86_64/fpu.h>
#include <arch/x86_64/cpu.h>
#include <lib/string.h>

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_NE (1ull << 5)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define MXCSR_OFFSET      24
#define MXCSR_MASK_OFFSET 28
#define FPU_REGS_OFFSET   32
#define FPU_REGS_SIZE     384

static uint8_t initial_state[FPU_STATE_SIZE] ALIGNED(16);
static uint32_t mxcsr_mask = 0xFFBF;
static bool have_initial;

void fpu_save(void *area)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const void *area)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

void fpu_init_cpu(void)
{
    uint64_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    write_cr0(cr0);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    if (!have_initial) {
        fpu_save(initial_state);
        memset(initial_state + FPU_REGS_OFFSET, 0, FPU_REGS_SIZE);
        uint32_t mask;
        memcpy(&mask, initial_state + MXCSR_MASK_OFFSET, sizeof(mask));
        if (mask)
            mxcsr_mask = mask;
        have_initial = true;
    }
}

void fpu_init_state(void *area)
{
    memcpy(area, initial_state, FPU_STATE_SIZE);
}

void fpu_reset_current(void)
{
    fpu_restore(initial_state);
}

void fpu_sanitize(void *area)
{
    uint32_t mxcsr;
    memcpy(&mxcsr, (uint8_t *)area + MXCSR_OFFSET, sizeof(mxcsr));
    mxcsr &= mxcsr_mask;
    memcpy((uint8_t *)area + MXCSR_OFFSET, &mxcsr, sizeof(mxcsr));
}
