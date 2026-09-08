/*
 * ioapic.h - I/O APIC (o "PIC moderno"): recebe as linhas de interrupção
 * dos dispositivos (GSIs) e as entrega ao LAPIC de uma CPU com o vetor que
 * escolhermos. Suporta múltiplos I/O APICs (cada um cobre uma faixa de GSIs).
 */
#pragma once

#include <kernel/compiler.h>

/* Inicializa a partir do MADT. false se não há I/O APIC. */
bool ioapic_init(void);
bool ioapic_available(void);

/* Roteia um GSI para (vetor, LAPIC id). Entrada começa mascarada. */
bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint32_t lapic_id,
                      bool active_low, bool level_triggered);
void ioapic_set_mask_gsi(uint32_t gsi, bool masked);

/* Conveniência para IRQs ISA (0-15): aplica os overrides do MADT. */
bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t lapic_id);
void ioapic_set_mask_isa(uint8_t irq, bool masked);
uint32_t ioapic_isa_to_gsi(uint8_t irq);

void ioapic_dump(void);
