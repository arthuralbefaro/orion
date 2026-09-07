#pragma once

#include <kernel/compiler.h>

bool ioapic_init(void);
bool ioapic_available(void);

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint32_t lapic_id,
                      bool active_low, bool level_triggered);
void ioapic_set_mask_gsi(uint32_t gsi, bool masked);

bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t lapic_id);
void ioapic_set_mask_isa(uint8_t irq, bool masked);
uint32_t ioapic_isa_to_gsi(uint8_t irq);

void ioapic_dump(void);