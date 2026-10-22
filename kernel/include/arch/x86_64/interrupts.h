#pragma once

#include <kernel/compiler.h>

#define VECTOR_EXCEPTIONS_END 32
#define VECTOR_IRQ_BASE       32
#define VECTOR_IRQ_COUNT      16
#define VECTOR_LAPIC_TIMER    0x40
#define VECTOR_GSI_BASE       0x50
#define VECTOR_GSI_COUNT      32
#define VECTOR_MSI_BASE       0x80
#define VECTOR_MSI_COUNT      32
#define VECTOR_IPI_HALT       0xF0
#define VECTOR_IPI_TLB        0xF1
#define VECTOR_IPI_TLB_SYNC   0xF2
#define VECTOR_IPI_RESCHED    0xF3
#define VECTOR_SPURIOUS       0xFF

#define EXC_DIVIDE_ERROR      0
#define EXC_DEBUG             1
#define EXC_NMI               2
#define EXC_BREAKPOINT        3
#define EXC_INVALID_OPCODE    6
#define EXC_DOUBLE_FAULT      8
#define EXC_GENERAL_PROTECTION 13
#define EXC_PAGE_FAULT        14
#define EXC_MACHINE_CHECK     18

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame, void *ctx);

void idt_init(void);
void idt_load(void);

void interrupts_register(uint8_t vector, interrupt_handler_t handler, void *ctx);
void interrupts_unregister(uint8_t vector);

enum irq_mode { IRQ_MODE_PIC, IRQ_MODE_IOAPIC };
void irq_set_mode(enum irq_mode mode);
enum irq_mode irq_get_mode(void);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);
bool irq_attach_gsi(uint32_t gsi, bool active_low, bool level, interrupt_handler_t handler, void *ctx);

uint64_t interrupts_count(uint8_t vector);
uint64_t interrupts_count_cpu(uint8_t vector, uint32_t cpu);
int interrupts_alloc_vector(interrupt_handler_t handler, void *ctx, const char *name);
const char *interrupts_name(uint8_t vector);
uint32_t irq_next_cpu(void);

const char *exception_name(uint8_t vector);

#define IRQ_NEST_LIMIT  8
#define IRQ_STORM_LIMIT 32768

bool irq_enable_isa_as(uint8_t irq, bool default_low, bool default_level, uint32_t *gsi, bool *low, bool *level);
bool interrupts_storm_masked(uint8_t vector);
void interrupts_storm_clear(uint8_t vector);
uint32_t interrupts_storm_events(void);
uint32_t interrupts_peak_rate(uint8_t vector);
int interrupts_storm_selftest(void);

void interrupts_check_entry(const struct interrupt_frame *f, const char *how);
void interrupts_check_user_return(struct interrupt_frame *f, const char *where);
void interrupts_user_iret_stats(uint64_t *checks, uint64_t *fixed);
void interrupts_dump(const struct interrupt_frame *f);
void interrupts_dump_cpu_state(uint32_t cpu);
void interrupts_dump_cpus(void);
void interrupts_backtrace_here(void);
