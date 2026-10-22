#include <drivers/pci.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>
#include <mm/vmm.h>
#include <kernel/log.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) == 0xFFFF)
        return;
    if (device_count >= PCI_MAX_DEVICES)
        return;

    struct pci_device *d = &devices[device_count++];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = pci_read32(bus, slot, func, 0x08);
    d->class_code = cls >> 24;
    d->subclass = (cls >> 16) & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, 0x10 + i * 4);
}

void pci_init(void)
{
    device_count = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            uint32_t hdr = pci_read32((uint8_t)bus, slot, 0, 0x0C);
            uint8_t funcs = (hdr & 0x00800000) ? 8 : 1;
            for (uint8_t f = 0; f < funcs; f++)
                probe((uint8_t)bus, slot, f);
        }
    }
    log_info("pci", "%u dispositivo(s) encontrado(s)", device_count);
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint32_t index)
{
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        if (d->class_code == class_code && d->subclass == subclass) {
            if (index == 0)
                return d;
            index--;
        }
    }
    return NULL;
}

void pci_enable_bus_master(const struct pci_device *dev)
{
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

uint8_t pci_find_cap(const struct pci_device *dev, uint8_t id)
{
    uint32_t status = pci_read32(dev->bus, dev->slot, dev->func, 0x04) >> 16;
    if (!(status & (1u << 4)))
        return 0;
    uint8_t ptr = pci_read32(dev->bus, dev->slot, dev->func, 0x34) & 0xFC;
    for (int hops = 0; ptr && hops < 48; hops++) {
        uint32_t head = pci_read32(dev->bus, dev->slot, dev->func, ptr);
        if ((head & 0xFF) == id)
            return ptr;
        ptr = (head >> 8) & 0xFC;
    }
    return 0;
}

bool pci_enable_msi(const struct pci_device *dev, uint8_t vector, uint32_t cpu)
{
    uint8_t off = pci_find_cap(dev, 0x05);
    struct percpu *c = percpu_by_index(cpu);
    if (!off || !c)
        return false;
    uint32_t head = pci_read32(dev->bus, dev->slot, dev->func, off);
    uint32_t ctl = head >> 16;
    bool wide = (ctl & (1u << 7)) != 0;
    pci_write32(dev->bus, dev->slot, dev->func, off + 4, 0xFEE00000u | (c->lapic_id << 12));
    uint8_t data_off = off + 8;
    if (wide) {
        pci_write32(dev->bus, dev->slot, dev->func, off + 8, 0);
        data_off = off + 12;
    }
    uint32_t data = pci_read32(dev->bus, dev->slot, dev->func, data_off);
    pci_write32(dev->bus, dev->slot, dev->func, data_off, (data & 0xFFFF0000u) | vector);
    ctl = (ctl & ~(7u << 4)) | 1u;
    pci_write32(dev->bus, dev->slot, dev->func, off, (head & 0xFFFF) | (ctl << 16));
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04) & 0xFFFF;
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd | (1u << 10) | (1u << 2) | (1u << 1));
    return true;
}

const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device, uint32_t index)
{
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        if (d->vendor == vendor && d->device == device) {
            if (index == 0)
                return d;
            index--;
        }
    }
    return NULL;
}

uint64_t pci_bar_address(const struct pci_device *dev, uint8_t bar)
{
    if (bar > 5)
        return 0;
    uint32_t lo = dev->bar[bar];
    if (lo & 1)
        return 0;
    uint64_t addr = lo & ~0xFull;
    if (((lo >> 1) & 3) == 2 && bar < 5)
        addr |= (uint64_t)dev->bar[bar + 1] << 32;
    return addr;
}

bool pci_enable_msix(const struct pci_device *dev, uint16_t entry, uint8_t vector, uint32_t cpu)
{
    uint8_t off = pci_find_cap(dev, 0x11);
    struct percpu *c = percpu_by_index(cpu);
    if (!off || !c)
        return false;
    uint32_t head = pci_read32(dev->bus, dev->slot, dev->func, off);
    uint32_t ctl = head >> 16;
    uint32_t size = (ctl & 0x7FF) + 1;
    if (entry >= size)
        return false;
    uint32_t table = pci_read32(dev->bus, dev->slot, dev->func, off + 4);
    uint64_t base = pci_bar_address(dev, (uint8_t)(table & 7));
    if (!base)
        return false;
    volatile uint32_t *e = vmm_map_mmio(base + (table & ~7u) + (uint64_t)entry * 16, 16);
    pci_write32(dev->bus, dev->slot, dev->func, off, (head & 0xFFFF) | ((ctl | (1u << 14) | (1u << 15)) << 16));
    e[0] = 0xFEE00000u | (c->lapic_id << 12);
    e[1] = 0;
    e[2] = vector;
    e[3] = 0;
    ctl = (ctl | (1u << 15)) & ~(1u << 14);
    pci_write32(dev->bus, dev->slot, dev->func, off, (head & 0xFFFF) | (ctl << 16));
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04) & 0xFFFF;
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd | (1u << 10) | (1u << 2) | (1u << 1));
    return true;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x01: return s == 0x06 ? "SATA (AHCI)" : s == 0x01 ? "IDE" : s == 0x08 ? "NVMe" : "storage";
    case 0x02: return "rede";
    case 0x03: return "video";
    case 0x06: return s == 0x00 ? "host bridge" : s == 0x01 ? "ISA bridge" : s == 0x04 ? "PCI bridge" : "bridge";
    case 0x0C: return s == 0x03 ? "USB" : s == 0x05 ? "SMBus" : "serial bus";
    default:   return "outro";
    }
}

void pci_set_driver(const struct pci_device *dev, const char *name)
{
    for (uint32_t i = 0; i < device_count; i++)
        if (&devices[i] == dev)
            devices[i].driver = name;
}

uint32_t pci_count(void)
{
    return device_count;
}

const struct pci_device *pci_at(uint32_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

void pci_dump(void)
{
    kprintf("  bus:slot.f  vendor:dev   classe                driver\n");
    for (uint32_t i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        kprintf("  %02x:%02x.%u     %04x:%04x    %02x.%02x %-14s %s\n", d->bus, d->slot, d->func,
                d->vendor, d->device, d->class_code, d->subclass,
                pci_class_name(d->class_code, d->subclass), d->driver ? d->driver : "nenhum");
    }
}
