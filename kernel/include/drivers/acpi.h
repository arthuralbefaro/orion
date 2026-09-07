#pragma once

#include <kernel/compiler.h>

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} PACKED;

#define ACPI_MAX_CPUS     64
#define ACPI_MAX_IOAPICS  8

struct acpi_cpu {
    uint32_t acpi_id;
    uint32_t apic_id;
    bool     enabled;
};

struct acpi_ioapic {
    uint8_t  id;
    uint32_t address;
    uint32_t gsi_base;
};

struct acpi_irq_override {
    bool     present;
    uint32_t gsi;
    bool     active_low;
    bool     level_triggered;
};

struct acpi_madt_info {
    uint32_t lapic_address;
    bool     has_legacy_pic;
    uint32_t cpu_count;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    uint32_t ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    struct acpi_irq_override isa_overrides[16];
};

bool acpi_init(void);
bool acpi_available(void);

const struct acpi_sdt_header *acpi_find_table(const char *signature);

const struct acpi_madt_info *acpi_madt(void);

void acpi_dump(void);