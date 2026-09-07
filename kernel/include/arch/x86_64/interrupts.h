#pragma once

#include <kernel/compiler.h>

#define VECTOR_EXCEPTIONS_END 32
#define VECTOR_IRQ_BASE       32
#define VECTOR_IRQ_COUNT      16
#define VECTOR_LAPIC_TIMER    0x40
#define VECTOR_IPI_HALT       0xF0
#define VECTOR_IPI_TLB        0xF1
#define VECTOR_SPURIOUS       0xFF

#define EXC_DIVIDE_ERROR      0
#define EXC_DEBUG             1
#define EXC_NMI               2
#define EXC_BREAKPOINT        3
#define EXC_INVALID_OPCODE    6
#define EXC_DOUBLE_FAULT      8
#define EXC_GENERAL_PROTECTION 13
#define EXC_PAGE_FAULT        14

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

enum irq_mode { IRQ_MODE_PIC, IRQ_MODE_IOAPIC };
void irq_set_mode(enum irq_mode mode);
enum irq_mode irq_get_mode(void);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame, void *ctx);

void idt_init(void);
void idt_load(void);

void interrupts_register(uint8_t vector, interrupt_handler_t handler, void *ctx);
void interrupts_unregister(uint8_t vector);

uint64_t interrupts_count(uint8_t vector);

const char *exception_name(uint8_t vector);