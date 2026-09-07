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

void pic_disable(void);