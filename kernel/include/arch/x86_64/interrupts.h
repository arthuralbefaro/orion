/*
 * interrupts.h - IDT, frame de interrupção e registro de handlers.
 *
 * Fluxo: CPU -> stub em isr_stubs.S (salva registradores) -> isr_dispatch()
 * -> handler registrado para o vetor (ou tratamento de exceção).
 */
#pragma once

#include <kernel/compiler.h>

/* Vetores */
#define VECTOR_EXCEPTIONS_END 32     /* 0..31 são exceções da CPU */
#define VECTOR_IRQ_BASE       32     /* IRQs do PIC remapeadas para 32..47 */
#define VECTOR_IRQ_COUNT      16
#define VECTOR_LAPIC_TIMER    0x40   /* timer do LAPIC */
#define VECTOR_IPI_HALT       0xF0
#define VECTOR_IPI_TLB        0xF1
#define VECTOR_SPURIOUS       0xFF   /* vetor espúrio do LAPIC */

/* Exceções mais usadas */
#define EXC_DIVIDE_ERROR      0
#define EXC_DEBUG             1
#define EXC_NMI               2
#define EXC_BREAKPOINT        3
#define EXC_INVALID_OPCODE    6
#define EXC_DOUBLE_FAULT      8
#define EXC_GENERAL_PROTECTION 13
#define EXC_PAGE_FAULT        14

/*
 * O que está na pilha quando o handler em C é chamado. A ordem DEVE bater
 * com os pushes de isr_stubs.S (do endereço mais baixo para o mais alto).
 */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;    /* número do vetor */
    uint64_t err_code;  /* código de erro (0 se a exceção não gera um) */
    /* empilhados pela CPU: */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame, void *ctx);

void idt_init(void);
void idt_load(void);

/* Registra um handler para um vetor (substitui o anterior). */
void interrupts_register(uint8_t vector, interrupt_handler_t handler, void *ctx);
void interrupts_unregister(uint8_t vector);

/*
 * IRQs de dispositivos (numeração ISA 0-15). Quem entrega ao LAPIC pode ser
 * o 8259 (modo PIC) ou o I/O APIC; os drivers não precisam saber qual.
 */
enum irq_mode { IRQ_MODE_PIC, IRQ_MODE_IOAPIC };
void irq_set_mode(enum irq_mode mode);
enum irq_mode irq_get_mode(void);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);

/* Estatísticas simples (para o comando "irq" do shell). */
uint64_t interrupts_count(uint8_t vector);

/* Nome legível de uma exceção. */
const char *exception_name(uint8_t vector);
