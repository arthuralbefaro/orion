/*
 * ioapic.c - driver do I/O APIC (82093AA e compatíveis).
 *
 * Acesso é indireto: escreve-se o índice do registrador em IOREGSEL e
 * lê/escreve-se o valor em IOWIN. Cada entrada da tabela de redirecionamento
 * (IOREDTBL) tem 64 bits, em dois registradores de 32.
 */
#include <drivers/ioapic.h>
#include <drivers/acpi.h>
#include <mm/vmm.h>
#include <kernel/log.h>

#define IOREGSEL 0x00
#define IOWIN    0x10

#define REG_ID      0x00
#define REG_VERSION 0x01
#define REG_REDTBL  0x10

#define RED_MASKED     (1u << 16)
#define RED_LEVEL      (1u << 15)
#define RED_ACTIVE_LOW (1u << 13)

struct ioapic {
    volatile uint8_t *base;   /* MMIO mapeado */
    uint32_t phys;
    uint32_t gsi_base;
    uint32_t gsi_count;
    uint8_t  id;
    uint8_t  version;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static uint32_t ioapic_count;

static inline uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
    *(volatile uint32_t *)(io->base + IOREGSEL) = reg;
    return *(volatile uint32_t *)(io->base + IOWIN);
}

static inline void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(io->base + IOREGSEL) = reg;
    *(volatile uint32_t *)(io->base + IOWIN) = value;
}

static struct ioapic *ioapic_for_gsi(uint32_t gsi)
{
    for (uint32_t i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->gsi_count)
            return io;
    }
    return NULL;
}

bool ioapic_init(void)
{
    const struct acpi_madt_info *madt = acpi_madt();
    ioapic_count = 0;
    if (!madt || madt->ioapic_count == 0)
        return false;

    for (uint32_t i = 0; i < madt->ioapic_count; i++) {
        struct ioapic *io = &ioapics[ioapic_count];
        io->phys = madt->ioapics[i].address;
        io->gsi_base = madt->ioapics[i].gsi_base;
        io->base = vmm_map_mmio(io->phys, 0x20);

        uint32_t ver = ioapic_read(io, REG_VERSION);
        io->version = ver & 0xFF;
        io->gsi_count = ((ver >> 16) & 0xFF) + 1;
        io->id = (ioapic_read(io, REG_ID) >> 24) & 0xF;

        /* Começa com tudo mascarado. */
        for (uint32_t e = 0; e < io->gsi_count; e++) {
            ioapic_write(io, REG_REDTBL + e * 2, RED_MASKED);
            ioapic_write(io, REG_REDTBL + e * 2 + 1, 0);
        }

        log_info("ioapic", "I/O APIC %u em %08x (mapeado em %p): GSIs %u-%u, versao 0x%02x",
                 io->id, io->phys, (void *)io->base, io->gsi_base,
                 io->gsi_base + io->gsi_count - 1, io->version);
        ioapic_count++;
    }
    return true;
}

bool ioapic_available(void)
{
    return ioapic_count > 0;
}

bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint32_t lapic_id,
                      bool active_low, bool level_triggered)
{
    struct ioapic *io = ioapic_for_gsi(gsi);
    if (!io) {
        log_error("ioapic", "GSI %u nao pertence a nenhum I/O APIC", gsi);
        return false;
    }
    uint32_t idx = gsi - io->gsi_base;
    uint32_t lo = vector | RED_MASKED;        /* fixed delivery, physical dest */
    if (active_low)      lo |= RED_ACTIVE_LOW;
    if (level_triggered) lo |= RED_LEVEL;
    ioapic_write(io, REG_REDTBL + idx * 2 + 1, lapic_id << 24);
    ioapic_write(io, REG_REDTBL + idx * 2, lo);
    return true;
}

void ioapic_set_mask_gsi(uint32_t gsi, bool masked)
{
    struct ioapic *io = ioapic_for_gsi(gsi);
    if (!io)
        return;
    uint32_t idx = gsi - io->gsi_base;
    uint32_t lo = ioapic_read(io, REG_REDTBL + idx * 2);
    if (masked) lo |= RED_MASKED; else lo &= ~RED_MASKED;
    ioapic_write(io, REG_REDTBL + idx * 2, lo);
}

uint32_t ioapic_isa_to_gsi(uint8_t irq)
{
    const struct acpi_madt_info *madt = acpi_madt();
    if (madt && irq < 16 && madt->isa_overrides[irq].present)
        return madt->isa_overrides[irq].gsi;
    return irq; /* sem override: IRQ n = GSI n */
}

bool ioapic_route_isa(uint8_t irq, uint8_t vector, uint32_t lapic_id)
{
    const struct acpi_madt_info *madt = acpi_madt();
    bool low = false, level = false;
    if (madt && irq < 16 && madt->isa_overrides[irq].present) {
        low = madt->isa_overrides[irq].active_low;
        level = madt->isa_overrides[irq].level_triggered;
    }
    return ioapic_route_gsi(ioapic_isa_to_gsi(irq), vector, lapic_id, low, level);
}

void ioapic_set_mask_isa(uint8_t irq, bool masked)
{
    ioapic_set_mask_gsi(ioapic_isa_to_gsi(irq), masked);
}

void ioapic_dump(void)
{
    for (uint32_t i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        kprintf("I/O APIC %u: GSIs %u-%u\n", io->id, io->gsi_base, io->gsi_base + io->gsi_count - 1);
        for (uint32_t e = 0; e < io->gsi_count; e++) {
            uint32_t lo = ioapic_read(io, REG_REDTBL + e * 2);
            uint32_t hi = ioapic_read(io, REG_REDTBL + e * 2 + 1);
            if (lo & RED_MASKED)
                continue;
            kprintf("  GSI %2u -> vetor %3u, LAPIC %u, %s, %s\n", io->gsi_base + e, lo & 0xFF,
                    hi >> 24, (lo & RED_ACTIVE_LOW) ? "low" : "high",
                    (lo & RED_LEVEL) ? "level" : "edge");
        }
    }
}
