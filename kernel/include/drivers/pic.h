/*
 * pic.h - controlador de interrupções legado 8259A (master + slave).
 *
 * Remapeamos as IRQs 0-15 para os vetores 32-47 (por padrão o firmware as
 * deixa em 8-15, colidindo com exceções da CPU).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_CASCADE   2
#define IRQ_COM2      3
#define IRQ_COM1      4
#define IRQ_RTC       8
#define IRQ_MOUSE     12

void pic_init(void);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_send_eoi(uint8_t irq);
bool pic_is_spurious(uint8_t irq);
uint16_t pic_get_mask(void);

/* Desliga o PIC por completo (quando migrarmos para o I/O APIC). */
void pic_disable(void);
