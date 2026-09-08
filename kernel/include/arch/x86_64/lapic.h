/*
 * lapic.h - Local APIC (x2APIC via MSRs, ou xAPIC via MMIO).
 *
 * Por que já mexer no LAPIC num kernel deste tamanho? Porque o protocolo
 * Limine (base revision 5+) entrega a CPU com o LAPIC ligado e TODAS as
 * entradas LVT mascaradas — inclusive a LINT0, que é por onde as interrupções
 * do PIC legado chegam ("virtual wire mode"). Se não reprogramarmos a LINT0
 * como ExtINT, PIT e teclado nunca interrompem.
 *
 * No x2APIC os registradores são MSRs 0x800+; no xAPIC ficam numa página
 * MMIO (16 bytes por registrador), que mapeamos pela janela do VMM.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* MSRs do x2APIC = 0x800 + (offset MMIO / 16) */
#define X2APIC_MSR_BASE   0x800
#define LAPIC_REG_ID      0x02
#define LAPIC_REG_VERSION 0x03
#define LAPIC_REG_TPR     0x08
#define LAPIC_REG_EOI     0x0B
#define LAPIC_REG_SVR     0x0F
#define LAPIC_REG_ICR     0x30
#define LAPIC_REG_LVT_TIMER   0x32
#define LAPIC_REG_LVT_LINT0   0x35
#define LAPIC_REG_LVT_LINT1   0x36
#define LAPIC_REG_LVT_ERROR   0x37
#define LAPIC_REG_TIMER_INIT  0x38
#define LAPIC_REG_TIMER_CUR   0x39
#define LAPIC_REG_TIMER_DIV   0x3E

#define LAPIC_LVT_MASKED      (1u << 16)
#define LAPIC_LVT_TIMER_PERIODIC (1u << 17)
#define LAPIC_LVT_DELIVERY_EXTINT (0x7u << 8)
#define LAPIC_LVT_DELIVERY_NMI    (0x4u << 8)
#define LAPIC_SVR_ENABLE      (1u << 8)

/* Habilita o LAPIC e configura LINT0 como ExtINT. false se não há APIC. */
bool lapic_init(void);
void lapic_enable_this_cpu(void);
bool lapic_is_enabled(void);
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);
void lapic_send_ipi_others(uint8_t vector);
bool lapic_is_x2apic(void);
uint32_t lapic_id(void);
void lapic_eoi(void);

/* LINT0 = ExtINT (IRQs do 8259 chegam por aqui) ou mascarada (modo I/O APIC). */
void lapic_set_lint0_extint(bool enable);

/* Timer do LAPIC. Calibra contra o PIT; retorna false se deu errado. */
bool     lapic_timer_calibrate(void);
uint64_t lapic_timer_frequency(void);   /* ticks do contador por segundo */
void     lapic_timer_start_periodic(uint8_t vector, uint32_t hz);
void     lapic_timer_stop(void);
