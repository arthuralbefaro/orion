#pragma once

#include <kernel/compiler.h>

struct pci_irq_route {
    uint32_t gsi;
    bool level;
    bool active_low;
};

void acpi_os_init(void);
void power_init(void);
bool power_ready(void);
bool power_pci_route(uint8_t slot, uint8_t pin, struct pci_irq_route *out);
NORETURN void power_off(void);
NORETURN void power_reboot(void);
NORETURN void power_halt(void);
