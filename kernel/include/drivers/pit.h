/*
 * pit.h - Programmable Interval Timer (Intel 8253/8254).
 *
 * Papel atual: (1) referência de tempo para calibrar o LAPIC timer e o TSC,
 * via canal 2 em polling; (2) fonte de tick de reserva (canal 0 -> IRQ0)
 * quando não há APIC.
 */
#pragma once

#include <stdint.h>

#define PIT_BASE_FREQUENCY 1193182u

/* Programa o canal 0 para gerar IRQ0 a `hz` e habilita a IRQ.
 * O handler é registrado por quem chama (ver kernel/time.c). */
void pit_init(uint32_t hz);

/* Espera `ms` (até ~50) usando o canal 2 em polling. Não usa interrupções. */
void pit_channel2_wait_ms(uint32_t ms);
