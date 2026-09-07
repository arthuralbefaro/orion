#pragma once

#include <stdbool.h>
#include <stdint.h>

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

bool lapic_init(void);
bool lapic_is_enabled(void);
bool lapic_is_x2apic(void);
uint32_t lapic_id(void);
void lapic_eoi(void);
void lapic_set_lint0_extint(bool enable);

bool     lapic_timer_calibrate(void);
uint64_t lapic_timer_frequency(void);
void     lapic_timer_start_periodic(uint8_t vector, uint32_t hz);
void     lapic_timer_stop(void);