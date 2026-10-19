#include <drivers/virtio.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <lib/string.h>

#define CAP_VENDOR       0x09
#define CFG_COMMON       1
#define CFG_NOTIFY       2
#define CFG_ISR          3
#define CFG_DEVICE       4

#define C_DFSELECT       0x00
#define C_DF             0x04
#define C_GFSELECT       0x08
#define C_GF             0x0C
#define C_MSIX_CONFIG    0x10
#define C_STATUS         0x14
#define C_Q_SELECT       0x16
#define C_Q_SIZE         0x18
#define C_Q_MSIX         0x1A
#define C_Q_ENABLE       0x1C
#define C_Q_NOFF         0x1E
#define C_Q_DESC         0x20
#define C_Q_DRIVER       0x28
#define C_Q_DEVICE       0x30

#define S_ACKNOWLEDGE    1
#define S_DRIVER         2
#define S_DRIVER_OK      4
#define S_FEATURES_OK    8
#define S_FAILED         128

#define NO_VECTOR        0xFFFF

static inline uint8_t r8(volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t *)(b + o); }
static inline uint16_t r16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t r32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void w8(volatile uint8_t *b, uint32_t o, uint8_t v) { *(volatile uint8_t *)(b + o) = v; }
static inline void w16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void w32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

static void w64(volatile uint8_t *b, uint32_t o, uint64_t v)
{
    w32(b, o, (uint32_t)v);
    w32(b, o + 4, (uint32_t)(v >> 32));
}

const struct pci_device *virtio_find(uint16_t modern_id, uint16_t legacy_id, uint32_t index)
{
    const struct pci_device *d = pci_find_id(VIRTIO_VENDOR, modern_id, index);
    return d ? d : pci_find_id(VIRTIO_VENDOR, legacy_id, index);
}

static volatile uint8_t *map_cap(const struct pci_device *pci, uint8_t off)
{
    uint8_t bar = pci_read32(pci->bus, pci->slot, pci->func, off + 4) & 0xFF;
    uint32_t offset = pci_read32(pci->bus, pci->slot, pci->func, off + 8);
    uint32_t length = pci_read32(pci->bus, pci->slot, pci->func, off + 12);
    uint64_t base = pci_bar_address(pci, bar);
    if (!base || !length)
        return NULL;
    return vmm_map_mmio(base + offset, length);
}

bool virtio_open(struct virtio_dev *v, const struct pci_device *pci, uint64_t wanted)
{
    memset(v, 0, sizeof(*v));
    v->pci = pci;
    uint32_t status = pci_read32(pci->bus, pci->slot, pci->func, 0x04) >> 16;
    uint8_t off = (status & (1u << 4)) ? pci_read32(pci->bus, pci->slot, pci->func, 0x34) & 0xFC : 0;
    for (int hops = 0; off && hops < 48; hops++) {
        uint32_t head = pci_read32(pci->bus, pci->slot, pci->func, off);
        if ((head & 0xFF) == CAP_VENDOR) {
            uint8_t type = (head >> 24) & 0xFF;
            if (type == CFG_COMMON && !v->common)
                v->common = map_cap(pci, off);
            else if (type == CFG_ISR && !v->isr)
                v->isr = map_cap(pci, off);
            else if (type == CFG_DEVICE && !v->device)
                v->device = map_cap(pci, off);
            else if (type == CFG_NOTIFY && !v->notify_base) {
                v->notify_base = map_cap(pci, off);
                v->notify_mult = pci_read32(pci->bus, pci->slot, pci->func, off + 16);
            }
        }
        off = (head >> 8) & 0xFC;
    }
    if (!v->common || !v->notify_base || !v->device)
        return false;
    pci_enable_bus_master(pci);

    w8(v->common, C_STATUS, 0);
    while (r8(v->common, C_STATUS))
        pause();
    w8(v->common, C_STATUS, S_ACKNOWLEDGE);
    w8(v->common, C_STATUS, S_ACKNOWLEDGE | S_DRIVER);

    w32(v->common, C_DFSELECT, 0);
    uint64_t offered = r32(v->common, C_DF);
    w32(v->common, C_DFSELECT, 1);
    offered |= (uint64_t)r32(v->common, C_DF) << 32;
    if (!(offered & VIRTIO_F_VERSION_1)) {
        virtio_fail(v);
        return false;
    }
    v->features = offered & (wanted | VIRTIO_F_VERSION_1);
    w32(v->common, C_GFSELECT, 0);
    w32(v->common, C_GF, (uint32_t)v->features);
    w32(v->common, C_GFSELECT, 1);
    w32(v->common, C_GF, (uint32_t)(v->features >> 32));
    w8(v->common, C_STATUS, S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK);
    if (!(r8(v->common, C_STATUS) & S_FEATURES_OK)) {
        virtio_fail(v);
        return false;
    }
    w16(v->common, C_MSIX_CONFIG, NO_VECTOR);
    return true;
}

bool virtio_queue(struct virtio_dev *v, uint16_t index, uint16_t max, uint16_t msix_entry, struct virtq *q)
{
    memset(q, 0, sizeof(*q));
    w16(v->common, C_Q_SELECT, index);
    uint16_t size = r16(v->common, C_Q_SIZE);
    if (!size)
        return false;
    if (size > max)
        size = max;
    w16(v->common, C_Q_SIZE, size);

    uint64_t desc_bytes = (uint64_t)size * sizeof(struct virtq_desc);
    uint64_t avail_bytes = 6 + 2ull * size;
    uint64_t used_off = PAGE_ALIGN_UP(desc_bytes + avail_bytes);
    uint64_t used_bytes = 6 + 8ull * size;
    uint64_t pages = PAGE_ALIGN_UP(used_off + used_bytes) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys)
        return false;
    uint8_t *mem = phys_to_virt(phys);
    memset(mem, 0, pages * PAGE_SIZE);

    q->index = index;
    q->size = size;
    q->desc = (struct virtq_desc *)mem;
    q->avail = (volatile uint16_t *)(mem + desc_bytes);
    q->used_idx = (volatile uint16_t *)(mem + used_off + 2);
    q->used_ring = (volatile struct virtq_used_elem *)(mem + used_off + 4);

    w64(v->common, C_Q_DESC, phys);
    w64(v->common, C_Q_DRIVER, phys + desc_bytes);
    w64(v->common, C_Q_DEVICE, phys + used_off);
    w16(v->common, C_Q_MSIX, msix_entry);
    if (msix_entry != NO_VECTOR && r16(v->common, C_Q_MSIX) != msix_entry)
        return false;
    uint16_t noff = r16(v->common, C_Q_NOFF);
    q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)noff * v->notify_mult);
    w16(v->common, C_Q_ENABLE, 1);
    return true;
}

bool virtio_irq(struct virtio_dev *v, uint16_t msix_entry, interrupt_handler_t fn, void *ctx, const char *name,
                int *vector, uint32_t *cpu)
{
    *vector = interrupts_alloc_vector(fn, ctx, name);
    *cpu = irq_next_cpu();
    if (*vector < 0)
        return false;
    if (!pci_enable_msix(v->pci, msix_entry, (uint8_t)*vector, *cpu)) {
        interrupts_unregister((uint8_t)*vector);
        *vector = -1;
        return false;
    }
    return true;
}

void virtio_ready(struct virtio_dev *v)
{
    w8(v->common, C_STATUS, r8(v->common, C_STATUS) | S_DRIVER_OK);
}

void virtio_fail(struct virtio_dev *v)
{
    w8(v->common, C_STATUS, r8(v->common, C_STATUS) | S_FAILED);
}

void virtq_push(struct virtq *q, uint16_t head)
{
    q->avail[2 + (q->avail_idx % q->size)] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = q->index;
}

bool virtq_pop(struct virtq *q, uint16_t *head, uint32_t *len)
{
    if (*q->used_idx == q->last_used)
        return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    volatile struct virtq_used_elem *e = &q->used_ring[q->last_used % q->size];
    *head = (uint16_t)e->id;
    *len = e->len;
    q->last_used++;
    return true;
}
