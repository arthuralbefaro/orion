/*
 * time.h - relógio do kernel.
 *
 * Fonte do tick (1000 Hz): timer do LAPIC (preferido, calibrado pelo PIT) ou
 * o PIT via IRQ0 (fallback em máquinas sem APIC/ACPI). Tempo fino vem do TSC.
 */
#pragma once

#include <kernel/compiler.h>

#define TIME_TICK_HZ 1000

void     time_init(void);
void     time_init_ap(void);
uint64_t time_ticks(void);           /* ticks desde o boot */
uint64_t time_uptime_ms(void);
uint64_t time_now_us(void);          /* microssegundos desde o boot (TSC) */
void     time_sleep_ms(uint64_t ms); /* bloqueante, requer interrupções */
void     time_udelay(uint64_t us);   /* espera ocupada, funciona sem IRQ */

const char *time_source_name(void);
uint64_t time_tsc_hz(void);
