#pragma once

#include <kernel/compiler.h>

struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class_code, subclass, prog_if;
    uint32_t bar[6];
};

#define PCI_MAX_DEVICES 64

void pci_init(void);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint32_t index);
void pci_enable_bus_master(const struct pci_device *dev);
uint8_t pci_find_cap(const struct pci_device *dev, uint8_t id);
bool pci_enable_msi(const struct pci_device *dev, uint8_t vector, uint32_t cpu);
void pci_dump(void);
