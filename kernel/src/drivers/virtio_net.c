#include <drivers/virtio.h>
#include <drivers/nic.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <lib/printf.h>

#define NET_HDR      12
#define BUF_SIZE     2048
#define QSIZE        64
#define F_MAC        (1ull << 5)
#define F_STATUS     (1ull << 16)
#define NO_VECTOR    0xFFFF

static struct virtio_dev dev;
static struct virtq rxq, txq;
static uint8_t *rx_buf[QSIZE];
static uint64_t rx_phys[QSIZE];
static uint8_t *tx_buf[QSIZE];
static uint64_t tx_phys[QSIZE];
static bool tx_busy[QSIZE];
static uint16_t tx_next;
static uint8_t mac[ETH_ALEN];
static struct nic_stats stats;
static spinlock_t lock = SPINLOCK_INIT;
static struct waitqueue rx_wq = WAITQUEUE_INIT;

static void vnet_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    stats.irqs++;
    wake_up_all(&rx_wq);
}

static void post_rx(uint16_t i)
{
    rxq.desc[i].addr = rx_phys[i];
    rxq.desc[i].len = BUF_SIZE;
    rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
    rxq.desc[i].next = 0;
    virtq_push(&rxq, i);
}

static const uint8_t *vnet_mac(void)
{
    return mac;
}

static bool vnet_link_up(void)
{
    if (!(dev.features & F_STATUS))
        return true;
    return (*(volatile uint16_t *)(dev.device + 6) & 1) != 0;
}

static int vnet_receive(void *buf, int max)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint16_t head;
    uint32_t len;
    if (!virtq_pop(&rxq, &head, &len)) {
        spin_unlock_irqrestore(&lock, flags);
        return 0;
    }
    int frame = (int)len - NET_HDR;
    if (head >= rxq.size || frame <= 0 || frame > max) {
        stats.rx_dropped++;
        frame = 0;
    } else {
        memcpy(buf, rx_buf[head] + NET_HDR, (size_t)frame);
        stats.rx_packets++;
    }
    if (head < rxq.size)
        post_rx(head);
    spin_unlock_irqrestore(&lock, flags);
    return frame;
}

static int vnet_send(const void *buf, int len)
{
    if (len <= 0 || len > ETH_FRAME_MAX)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&lock);
    uint16_t head;
    uint32_t used;
    while (virtq_pop(&txq, &head, &used))
        if (head < txq.size)
            tx_busy[head] = false;
    uint16_t slot = txq.size;
    for (uint16_t n = 0; n < txq.size; n++) {
        uint16_t i = (uint16_t)((tx_next + n) % txq.size);
        if (!tx_busy[i]) {
            slot = i;
            break;
        }
    }
    if (slot == txq.size) {
        stats.tx_dropped++;
        spin_unlock_irqrestore(&lock, flags);
        return -EAGAIN;
    }
    memset(tx_buf[slot], 0, NET_HDR);
    memcpy(tx_buf[slot] + NET_HDR, buf, (size_t)len);
    txq.desc[slot].addr = tx_phys[slot];
    txq.desc[slot].len = (uint32_t)(NET_HDR + len);
    txq.desc[slot].flags = 0;
    txq.desc[slot].next = 0;
    tx_busy[slot] = true;
    tx_next = (uint16_t)((slot + 1) % txq.size);
    virtq_push(&txq, slot);
    stats.tx_packets++;
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}

static void vnet_stats(struct nic_stats *out)
{
    *out = stats;
}

static struct waitqueue *vnet_rx_queue(void)
{
    return &rx_wq;
}

static const struct nic_ops vnet_ops = {
    .name = "virtio-net",
    .mac = vnet_mac,
    .link_up = vnet_link_up,
    .receive = vnet_receive,
    .send = vnet_send,
    .stats = vnet_stats,
    .rx_queue = vnet_rx_queue,
};

static bool alloc_buffers(uint8_t **bufs, uint64_t *phys, uint16_t count)
{
    uint64_t pages = PAGE_ALIGN_UP((uint64_t)count * BUF_SIZE) / PAGE_SIZE;
    uint64_t base = pmm_alloc_pages(pages);
    if (!base)
        return false;
    for (uint16_t i = 0; i < count; i++) {
        phys[i] = base + (uint64_t)i * BUF_SIZE;
        bufs[i] = phys_to_virt(phys[i]);
    }
    return true;
}

bool virtio_net_init(void)
{
    const struct pci_device *pci = virtio_find(0x1041, 0x1000, 0);
    if (!pci)
        return false;
    if (!virtio_open(&dev, pci, F_MAC | F_STATUS)) {
        log_warn("virtio", "virtio-net em %02x:%02x.%u nao iniciou", pci->bus, pci->slot, pci->func);
        return false;
    }
    int vector = -1;
    uint32_t cpu = 0;
    bool irq_on = virtio_irq(&dev, 0, vnet_irq, NULL, "virtio-net", &vector, &cpu);
    if (!virtio_queue(&dev, 0, QSIZE, irq_on ? 0 : NO_VECTOR, &rxq) ||
        !virtio_queue(&dev, 1, QSIZE, NO_VECTOR, &txq) || !alloc_buffers(rx_buf, rx_phys, rxq.size) ||
        !alloc_buffers(tx_buf, tx_phys, txq.size)) {
        virtio_fail(&dev);
        log_warn("virtio", "virtio-net: filas nao iniciaram");
        return false;
    }
    if (dev.features & F_MAC)
        for (int i = 0; i < ETH_ALEN; i++)
            mac[i] = dev.device[i];
    else
        memcpy(mac, (const uint8_t[]){ 0x52, 0x54, 0x00, 0x4F, 0x52, 0x4E }, ETH_ALEN);
    virtio_ready(&dev);
    uint64_t flags = spin_lock_irqsave(&lock);
    for (uint16_t i = 0; i < rxq.size; i++)
        post_rx(i);
    spin_unlock_irqrestore(&lock, flags);
    nic_register(&vnet_ops);

    char how[48];
    if (irq_on)
        ksnprintf(how, sizeof(how), "MSI-X no vetor %d, CPU %u", vector, cpu);
    else
        ksnprintf(how, sizeof(how), "sem MSI-X");
    log_info("virtio", "virtio-net %04x:%04x em %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x, filas de %u, %s, link %s",
             pci->vendor, pci->device, pci->bus, pci->slot, pci->func, mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5], rxq.size, how, vnet_link_up() ? "up" : "down");
    return true;
}
