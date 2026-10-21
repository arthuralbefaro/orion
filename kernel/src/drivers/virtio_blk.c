#include <drivers/virtio.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <arch/x86_64/cpu.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <lib/printf.h>

#define BLK_T_IN     0
#define BLK_T_OUT    1
#define DMA_PAGES    16
#define MAX_SECTORS  ((DMA_PAGES * PAGE_SIZE) / 512)
#define MAX_DISKS    4
#define NO_VECTOR    0xFFFF

struct blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} PACKED;

struct vblk {
    struct virtio_dev dev;
    struct virtq q;
    struct mutex lock;
    spinlock_t irq_lock;
    struct waitqueue wq;
    bool irq_on;
    uint64_t sectors;
    struct blk_req *hdr;
    uint64_t hdr_phys;
    volatile uint8_t *status;
    uint64_t status_phys;
    uint8_t *dma;
    uint64_t dma_phys;
    char lock_name[12];
};

static struct vblk disks[MAX_DISKS];
static uint32_t disk_count;

static void vblk_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    struct vblk *d = ctx;
    spin_lock(&d->irq_lock);
    wake_up_all(&d->wq);
    spin_unlock(&d->irq_lock);
}

static bool request(struct vblk *d, uint32_t type, uint64_t sector, uint32_t bytes)
{
    d->hdr->type = type;
    d->hdr->reserved = 0;
    d->hdr->sector = sector;
    *d->status = 0xFF;
    struct virtq_desc *desc = d->q.desc;
    desc[0].addr = d->hdr_phys;
    desc[0].len = sizeof(struct blk_req);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;
    desc[1].addr = d->dma_phys;
    desc[1].len = bytes;
    desc[1].flags = VIRTQ_DESC_F_NEXT | (type == BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    desc[1].next = 2;
    desc[2].addr = d->status_phys;
    desc[2].len = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0;
    virtq_push(&d->q, 0);

    uint64_t end = time_now_us() + 5000000;
    bool can_sleep = d->irq_on && interrupts_enabled() && sched_enabled();
    uint16_t head;
    uint32_t len;
    for (;;) {
        if (can_sleep) {
            uint64_t flags = spin_lock_irqsave(&d->irq_lock);
            if (*d->q.used_idx == d->q.last_used)
                wait_sleep_timeout(&d->wq, &d->irq_lock, 10);
            spin_unlock_irqrestore(&d->irq_lock, flags);
        }
        if (virtq_pop(&d->q, &head, &len))
            break;
        if (time_now_us() > end)
            return false;
        if (!can_sleep) {
            if (interrupts_enabled() && sched_enabled())
                thread_yield();
            else
                pause();
        }
    }
    return *d->status == 0;
}

static bool vblk_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct vblk *d = dev->priv;
    uint8_t *out = buf;
    mutex_lock(&d->lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        if (!request(d, BLK_T_IN, lba, n * 512)) {
            mutex_unlock(&d->lock);
            log_error("virtio", "%s: erro lendo LBA %lu", dev->name, lba);
            return false;
        }
        memcpy(out, d->dma, n * 512);
        out += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->lock);
    return true;
}

static bool vblk_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct vblk *d = dev->priv;
    const uint8_t *in = buf;
    mutex_lock(&d->lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        memcpy(d->dma, in, n * 512);
        if (!request(d, BLK_T_OUT, lba, n * 512)) {
            mutex_unlock(&d->lock);
            log_error("virtio", "%s: erro escrevendo LBA %lu", dev->name, lba);
            return false;
        }
        in += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->lock);
    return true;
}

static bool open_disk(struct vblk *d, const struct pci_device *pci)
{
    if (!virtio_open(&d->dev, pci, 0))
        return false;
    int vector = -1;
    uint32_t cpu = 0;
    d->irq_on = virtio_irq(&d->dev, 0, vblk_irq, d, "virtio-blk", &vector, &cpu);
    if (!virtio_queue(&d->dev, 0, 128, d->irq_on ? 0 : NO_VECTOR, &d->q)) {
        virtio_fail(&d->dev);
        return false;
    }
    volatile uint32_t *cfg = (volatile uint32_t *)d->dev.device;
    d->sectors = (uint64_t)cfg[0] | ((uint64_t)cfg[1] << 32);

    uint64_t meta = pmm_alloc_page();
    d->dma_phys = pmm_alloc_pages(DMA_PAGES);
    if (!meta || !d->dma_phys) {
        virtio_fail(&d->dev);
        return false;
    }
    uint8_t *m = phys_to_virt(meta);
    memset(m, 0, PAGE_SIZE);
    d->hdr = (struct blk_req *)m;
    d->hdr_phys = meta;
    d->status = m + 64;
    d->status_phys = meta + 64;
    d->dma = phys_to_virt(d->dma_phys);

    ksnprintf(d->lock_name, sizeof(d->lock_name), "vblk%u", disk_count);
    mutex_init(&d->lock, d->lock_name);
    d->irq_lock = (spinlock_t)SPINLOCK_INIT;
    waitqueue_init(&d->wq);
    virtio_ready(&d->dev);

    char how[48];
    if (d->irq_on)
        ksnprintf(how, sizeof(how), "MSI-X no vetor %d, CPU %u", vector, cpu);
    else
        ksnprintf(how, sizeof(how), "sem MSI-X, espera ativa");
    char name[BLOCK_NAME_MAX];
    ksnprintf(name, sizeof(name), "vd%c", 'a' + disk_count);
    struct blockdev *dev = blockdev_register(name, 512, d->sectors, vblk_read, vblk_write, d);
    if (!dev)
        return false;
    strlcpy(dev->model, "virtio-blk", sizeof(dev->model));
    log_info("virtio", "%s: virtio-blk %04x:%04x em %02x:%02x.%u, fila de %u, %lu setores (%lu MiB), %s", name,
             pci->vendor, pci->device, pci->bus, pci->slot, pci->func, d->q.size, d->sectors,
             d->sectors * 512 / MiB(1), how);
    disk_count++;
    blockdev_scan_partitions(dev);
    return true;
}

bool virtio_blk_init(void)
{
    for (uint32_t i = 0; i < MAX_DISKS * 2 && disk_count < MAX_DISKS; i++) {
        const struct pci_device *pci = virtio_find(0x1042, 0x1001, i);
        if (!pci)
            break;
        if (!open_disk(&disks[disk_count], pci))
            log_warn("virtio", "virtio-blk em %02x:%02x.%u nao iniciou", pci->bus, pci->slot, pci->func);
    }
    return disk_count > 0;
}
