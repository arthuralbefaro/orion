#pragma once

#include <drivers/pci.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/compiler.h>

#define VIRTIO_VENDOR        0x1AF4
#define VIRTQ_DESC_F_NEXT    1
#define VIRTQ_DESC_F_WRITE   2
#define VIRTIO_F_VERSION_1   (1ull << 32)

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} PACKED;

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} PACKED;

struct virtq {
    uint16_t index;
    uint16_t size;
    struct virtq_desc *desc;
    volatile uint16_t *avail;
    volatile uint16_t *used_idx;
    volatile struct virtq_used_elem *used_ring;
    volatile uint16_t *notify;
    uint16_t avail_idx;
    uint16_t last_used;
};

struct virtio_dev {
    const struct pci_device *pci;
    volatile uint8_t *common;
    volatile uint8_t *isr;
    volatile uint8_t *device;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    uint64_t features;
};

const struct pci_device *virtio_find(uint16_t modern_id, uint16_t legacy_id, uint32_t index);
bool virtio_open(struct virtio_dev *v, const struct pci_device *pci, uint64_t wanted);
bool virtio_queue(struct virtio_dev *v, uint16_t index, uint16_t max, uint16_t msix_entry, struct virtq *q);
bool virtio_irq(struct virtio_dev *v, uint16_t msix_entry, interrupt_handler_t fn, void *ctx, const char *name,
                int *vector, uint32_t *cpu);
void virtio_ready(struct virtio_dev *v);
void virtio_fail(struct virtio_dev *v);
void virtq_push(struct virtq *q, uint16_t head);
bool virtq_pop(struct virtq *q, uint16_t *head, uint32_t *len);

bool virtio_blk_init(void);
bool virtio_net_init(void);
