/*
 * acpi.c - RSDP, XSDT/RSDT e MADT.
 *
 * As tabelas ficam em regiões "ACPI reclaimable"/"NVS", que o Limine (base
 * revision 4+) já mapeia no HHDM. Por isso exigimos revision >= 4 aqui.
 */
#include <drivers/acpi.h>
#include <boot/bootinfo.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <kernel/log.h>

struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       /* 0 = ACPI 1.0, 2+ = ACPI 2.0+ */
    uint32_t rsdt_address;
    /* só se revision >= 2: */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} PACKED;

struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;          /* bit 0: PCAT_COMPAT (tem 8259) */
    uint8_t  entries[];
} PACKED;

struct madt_entry {
    uint8_t type;
    uint8_t length;
} PACKED;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_ISO            2   /* Interrupt Source Override */
#define MADT_LAPIC_NMI      4
#define MADT_LAPIC_OVERRIDE 5
#define MADT_X2APIC         9

#define MAX_TABLES 64
static const struct acpi_sdt_header *tables[MAX_TABLES];
static uint32_t table_count;
static bool available;
static struct acpi_madt_info madt_info;

static bool checksum_ok(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

static void parse_madt(const struct acpi_madt *m)
{
    memset(&madt_info, 0, sizeof(madt_info));
    madt_info.lapic_address = m->lapic_address;
    madt_info.has_legacy_pic = m->flags & 1;

    const uint8_t *p = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->hdr.length;

    while (p + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *e = (const void *)p;
        if (e->length < 2 || p + e->length > end)
            break;

        switch (e->type) {
        case MADT_LAPIC: {
            uint8_t acpi_id = p[2], apic_id = p[3];
            uint32_t flags;
            memcpy(&flags, p + 4, 4);
            if (madt_info.cpu_count < ACPI_MAX_CPUS) {
                struct acpi_cpu *c = &madt_info.cpus[madt_info.cpu_count++];
                c->acpi_id = acpi_id;
                c->apic_id = apic_id;
                c->enabled = (flags & 1) || (flags & 2); /* enabled | online capable */
            }
            break;
        }
        case MADT_X2APIC: {
            uint32_t apic_id, flags, acpi_id;
            memcpy(&apic_id, p + 4, 4);
            memcpy(&flags, p + 8, 4);
            memcpy(&acpi_id, p + 12, 4);
            if (madt_info.cpu_count < ACPI_MAX_CPUS) {
                struct acpi_cpu *c = &madt_info.cpus[madt_info.cpu_count++];
                c->acpi_id = acpi_id;
                c->apic_id = apic_id;
                c->enabled = (flags & 1) || (flags & 2);
            }
            break;
        }
        case MADT_IOAPIC: {
            if (madt_info.ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &madt_info.ioapics[madt_info.ioapic_count++];
                io->id = p[2];
                memcpy(&io->address, p + 4, 4);
                memcpy(&io->gsi_base, p + 8, 4);
            }
            break;
        }
        case MADT_ISO: {
            uint8_t source = p[3];
            uint32_t gsi;
            uint16_t flags;
            memcpy(&gsi, p + 4, 4);
            memcpy(&flags, p + 8, 2);
            if (source < 16) {
                struct acpi_irq_override *o = &madt_info.isa_overrides[source];
                o->present = true;
                o->gsi = gsi;
                o->active_low = (flags & 3) == 3;
                o->level_triggered = ((flags >> 2) & 3) == 3;
            }
            break;
        }
        case MADT_LAPIC_OVERRIDE: {
            uint64_t addr;
            memcpy(&addr, p + 4, 8);
            madt_info.lapic_address = (uint32_t)addr;
            break;
        }
        default:
            break;
        }
        p += e->length;
    }
}

bool acpi_init(void)
{
    available = false;
    table_count = 0;

    if (!boot_info.rsdp) {
        log_warn("acpi", "bootloader nao informou o RSDP");
        return false;
    }
    if (boot_info.base_revision < 4) {
        log_warn("acpi", "Limine rev %lu nao mapeia as tabelas ACPI; pulando", boot_info.base_revision);
        return false;
    }

    const struct acpi_rsdp *rsdp = boot_info.rsdp;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        log_warn("acpi", "RSDP invalido");
        return false;
    }

    const struct acpi_sdt_header *root;
    size_t entry_size;
    if (rsdp->revision >= 2 && rsdp->xsdt_address && checksum_ok(rsdp, rsdp->length)) {
        root = phys_to_virt(rsdp->xsdt_address);
        entry_size = 8;
    } else {
        root = phys_to_virt(rsdp->rsdt_address);
        entry_size = 4;
    }
    if (!checksum_ok(root, root->length)) {
        log_warn("acpi", "%s com checksum invalido", entry_size == 8 ? "XSDT" : "RSDT");
        return false;
    }

    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    size_t n = (root->length - sizeof(*root)) / entry_size;
    for (size_t i = 0; i < n && table_count < MAX_TABLES; i++) {
        uint64_t phys = 0;
        memcpy(&phys, entries + i * entry_size, entry_size);
        if (!phys)
            continue;
        const struct acpi_sdt_header *t = phys_to_virt(phys);
        if (!checksum_ok(t, t->length)) {
            log_warn("acpi", "tabela %.4s com checksum invalido, ignorada", t->signature);
            continue;
        }
        tables[table_count++] = t;
    }

    log_info("acpi", "%s rev %u, OEM '%.6s', %u tabelas via %s",
             rsdp->revision >= 2 ? "ACPI 2.0+" : "ACPI 1.0", rsdp->revision,
             rsdp->oem_id, table_count, entry_size == 8 ? "XSDT" : "RSDT");

    const struct acpi_madt *madt = (const struct acpi_madt *)acpi_find_table("APIC");
    if (!madt) {
        log_warn("acpi", "sem MADT; sem I/O APIC");
        return false;
    }
    parse_madt(madt);

    uint32_t enabled = 0;
    for (uint32_t i = 0; i < madt_info.cpu_count; i++)
        enabled += madt_info.cpus[i].enabled;
    log_info("acpi", "MADT: LAPIC em %08x, %u CPU(s) (%u ativas), %u I/O APIC(s), 8259 %s",
             madt_info.lapic_address, madt_info.cpu_count, enabled, madt_info.ioapic_count,
             madt_info.has_legacy_pic ? "presente" : "ausente");
    for (int i = 0; i < 16; i++) {
        const struct acpi_irq_override *o = &madt_info.isa_overrides[i];
        if (o->present)
            log_info("acpi", "  IRQ %d -> GSI %u (%s, %s)", i, o->gsi,
                     o->active_low ? "active low" : "active high",
                     o->level_triggered ? "level" : "edge");
    }

    available = true;
    return true;
}

bool acpi_available(void)
{
    return available;
}

const struct acpi_sdt_header *acpi_find_table(const char *signature)
{
    for (uint32_t i = 0; i < table_count; i++)
        if (memcmp(tables[i]->signature, signature, 4) == 0)
            return tables[i];
    return NULL;
}

const struct acpi_madt_info *acpi_madt(void)
{
    return available ? &madt_info : NULL;
}

void acpi_dump(void)
{
    if (!table_count) {
        kprintf("ACPI indisponivel.\n");
        return;
    }
    kprintf("  sig   rev  tamanho  OEM     tabela    endereco\n");
    for (uint32_t i = 0; i < table_count; i++) {
        const struct acpi_sdt_header *t = tables[i];
        kprintf("  %.4s  %3u  %7u  %.6s  %.8s  %016lx\n", t->signature, t->revision,
                t->length, t->oem_id, t->oem_table_id, virt_to_phys(t));
    }
}