#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <kernel/syscall.h>

#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL      0x5400
#define REG_RAH      0x5404

#define CTRL_SLU     (1u << 6)
#define CTRL_ASDE    (1u << 5)
#define CTRL_RST     (1u << 26)
#define STATUS_LU    (1u << 1)

#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)
#define RCTL_SECRC   (1u << 26)
#define RCTL_BSIZE_2048 0

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)
#define TCTL_CT      (0x0F << 4)
#define TCTL_COLD    (0x40 << 12)

#define ICR_RXT0     (1u << 7)
#define ICR_RXDMT0   (1u << 4)
#define ICR_LSC      (1u << 2)

#define TXD_CMD_EOP  (1u << 0)
#define TXD_CMD_IFCS (1u << 1)
#define TXD_CMD_RS   (1u << 3)
#define TXD_STA_DD   (1u << 0)
#define RXD_STA_DD   (1u << 0)
#define RXD_STA_EOP  (1u << 1)

#define RX_DESCS     32
#define TX_DESCS     32
#define RX_BUF_SIZE  2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} PACKED;

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} PACKED;

static volatile uint8_t *regs;
static struct rx_desc *rx_ring;
static struct tx_desc *tx_ring;
static uint64_t rx_ring_phys, tx_ring_phys;
static uint8_t *rx_buf[RX_DESCS];
static uint8_t *tx_buf[TX_DESCS];
static uint64_t tx_buf_phys[TX_DESCS];
static uint32_t rx_next, tx_next;
static uint8_t mac[ETH_ALEN];
static bool present;
static struct e1000_stats stats;
static spinlock_t e1000_lock = SPINLOCK_INIT;
static struct waitqueue rx_wq = WAITQUEUE_INIT;

static inline uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static inline void wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(regs + off) = val;
}

static void read_mac(void)
{
    uint32_t low = rd(REG_RAL);
    uint32_t high = rd(REG_RAH);
    mac[0] = (uint8_t)(low);
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)(high);
    mac[5] = (uint8_t)(high >> 8);
}

static void setup_rx(void)
{
    uint64_t ring_pages = PAGE_ALIGN_UP(RX_DESCS * sizeof(struct rx_desc)) / PAGE_SIZE;
    rx_ring_phys = pmm_alloc_pages(ring_pages);
    rx_ring = phys_to_virt(rx_ring_phys);
    memset(rx_ring, 0, ring_pages * PAGE_SIZE);

    uint64_t buf_pages = (RX_DESCS * RX_BUF_SIZE) / PAGE_SIZE;
    uint64_t buf_phys = pmm_alloc_pages(buf_pages);
    for (int i = 0; i < RX_DESCS; i++) {
        rx_ring[i].addr = buf_phys + (uint64_t)i * RX_BUF_SIZE;
        rx_ring[i].status = 0;
        rx_buf[i] = phys_to_virt(rx_ring[i].addr);
    }

    wr(REG_RDBAL, (uint32_t)rx_ring_phys);
    wr(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    wr(REG_RDLEN, RX_DESCS * (uint32_t)sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESCS - 1);
    rx_next = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void setup_tx(void)
{
    uint64_t ring_pages = PAGE_ALIGN_UP(TX_DESCS * sizeof(struct tx_desc)) / PAGE_SIZE;
    tx_ring_phys = pmm_alloc_pages(ring_pages);
    tx_ring = phys_to_virt(tx_ring_phys);
    memset(tx_ring, 0, ring_pages * PAGE_SIZE);

    uint64_t buf_pages = (TX_DESCS * RX_BUF_SIZE) / PAGE_SIZE;
    uint64_t buf_phys = pmm_alloc_pages(buf_pages);
    for (int i = 0; i < TX_DESCS; i++) {
        tx_buf_phys[i] = buf_phys + (uint64_t)i * RX_BUF_SIZE;
        tx_buf[i] = phys_to_virt(tx_buf_phys[i]);
        tx_ring[i].status = TXD_STA_DD;
    }

    wr(REG_TDBAL, (uint32_t)tx_ring_phys);
    wr(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    wr(REG_TDLEN, TX_DESCS * (uint32_t)sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_next = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));
}

static void e1000_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    uint32_t icr = rd(REG_ICR);
    if (!icr)
        return;
    stats.irqs++;
    if (icr & (ICR_RXT0 | ICR_RXDMT0))
        wake_up_all(&rx_wq);
}

bool e1000_init(void)
{
    const struct pci_device *dev = pci_find_class(0x02, 0x00, 0);
    if (!dev) {
        log_info("e1000", "nenhuma placa de rede ethernet no PCI");
        return false;
    }
    if (dev->vendor != 0x8086) {
        log_warn("e1000", "placa %04x:%04x nao suportada", dev->vendor, dev->device);
        return false;
    }
    pci_enable_bus_master(dev);
    regs = vmm_map_mmio(dev->bar[0] & ~0xFull, 0x20000);

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    uint64_t end = time_now_us() + 100000;
    while ((rd(REG_CTRL) & CTRL_RST) && time_now_us() < end)
        pause();
    wr(REG_IMC, 0xFFFFFFFF);
    (void)rd(REG_ICR);

    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (int i = 0; i < 128; i++)
        wr(REG_MTA + i * 4, 0);

    read_mac();
    setup_rx();
    setup_tx();

    uint8_t irq = (uint8_t)(pci_read32(dev->bus, dev->slot, dev->func, 0x3C) & 0xFF);
    if (irq && irq < 16) {
        interrupts_register((uint8_t)(VECTOR_IRQ_BASE + irq), e1000_irq, NULL);
        irq_enable(irq);
        wr(REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_LSC);
    }

    present = true;
    log_info("e1000", "%04x:%04x em %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x, IRQ %u, link %s",
             dev->vendor, dev->device, dev->bus, dev->slot, dev->func,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], irq,
             e1000_link_up() ? "up" : "down");
    return true;
}

bool e1000_present(void)
{
    return present;
}

const uint8_t *e1000_mac(void)
{
    return mac;
}

bool e1000_link_up(void)
{
    return present && (rd(REG_STATUS) & STATUS_LU) != 0;
}

int e1000_receive(void *buf, int max)
{
    if (!present)
        return 0;
    uint64_t flags = spin_lock_irqsave(&e1000_lock);
    struct rx_desc *d = &rx_ring[rx_next];
    if (!(d->status & RXD_STA_DD)) {
        spin_unlock_irqrestore(&e1000_lock, flags);
        return 0;
    }
    int len = d->length;
    if (len > max) {
        len = 0;
        stats.rx_dropped++;
    } else {
        memcpy(buf, rx_buf[rx_next], (size_t)len);
        stats.rx_packets++;
    }
    d->status = 0;
    uint32_t tail = rx_next;
    rx_next = (rx_next + 1) % RX_DESCS;
    wr(REG_RDT, tail);
    spin_unlock_irqrestore(&e1000_lock, flags);
    return len;
}

int e1000_send(const void *buf, int len)
{
    if (!present)
        return -ENODEV;
    if (len <= 0 || len > ETH_FRAME_MAX)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&e1000_lock);
    struct tx_desc *d = &tx_ring[tx_next];
    if (!(d->status & TXD_STA_DD)) {
        stats.tx_dropped++;
        spin_unlock_irqrestore(&e1000_lock, flags);
        return -EAGAIN;
    }
    memcpy(tx_buf[tx_next], buf, (size_t)len);
    d->addr = tx_buf_phys[tx_next];
    d->length = (uint16_t)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    tx_next = (tx_next + 1) % TX_DESCS;
    wr(REG_TDT, tx_next);
    stats.tx_packets++;
    spin_unlock_irqrestore(&e1000_lock, flags);
    return 0;
}

void e1000_get_stats(struct e1000_stats *out)
{
    *out = stats;
}

struct waitqueue *e1000_rx_queue(void)
{
    return &rx_wq;
}
