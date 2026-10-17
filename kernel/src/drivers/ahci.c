#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <lib/printf.h>

#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_PORTS 0x100
#define PORT_SIZE 0x80

#define PX_CLB  0x00
#define PX_CLBU 0x04
#define PX_FB   0x08
#define PX_FBU  0x0C
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI   0x38

#define GHC_AE      (1u << 31)
#define GHC_IE      (1u << 1)
#define CMD_ST      (1u << 0)
#define CMD_FRE     (1u << 4)
#define CMD_FR      (1u << 14)
#define CMD_CR      (1u << 15)
#define TFD_BSY     (1u << 7)
#define TFD_DRQ     (1u << 3)
#define TFD_ERR     (1u << 0)
#define SIG_SATA    0x00000101

#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_CACHE_EXT 0xEA
#define ATA_IDENTIFY     0xEC
#define FIS_TYPE_H2D     0x27

#define DMA_PAGES        16
#define MAX_SECTORS_PER_CMD ((DMA_PAGES * 4096) / 512)

struct cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} PACKED;

struct prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
} PACKED;

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prdt_entry prdt[8];
} PACKED;

struct ahci_port {
    volatile uint8_t *regs;
    uint32_t index;
    uint64_t mem_phys;
    struct cmd_header *cmd_list;
    struct cmd_table *cmd_table;
    uint64_t dma_phys;
    uint8_t *dma_buf;
    uint64_t sectors;
    char model[41];
    char lock_name[12];
    struct mutex lock;
};

static volatile uint8_t *abar;
static struct ahci_port ports[32];
static uint32_t port_count;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static bool wait_clear(volatile uint8_t *base, uint32_t off, uint32_t mask, uint64_t timeout_us)
{
    uint64_t end = time_now_us() + timeout_us;
    while (rd(base, off) & mask) {
        if (time_now_us() > end)
            return false;
        pause();
    }
    return true;
}

static void port_stop(struct ahci_port *p)
{
    uint32_t cmd = rd(p->regs, PX_CMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    wr(p->regs, PX_CMD, cmd);
    wait_clear(p->regs, PX_CMD, CMD_CR | CMD_FR, 500000);
}

static void port_start(struct ahci_port *p)
{
    wait_clear(p->regs, PX_CMD, CMD_CR, 500000);
    wr(p->regs, PX_IE, 0);
    wr(p->regs, PX_IS, 0xFFFFFFFF);
    wr(p->regs, PX_CMD, rd(p->regs, PX_CMD) | CMD_FRE);
    wr(p->regs, PX_CMD, rd(p->regs, PX_CMD) | CMD_ST);
}

static bool port_command(struct ahci_port *p, uint8_t ata_cmd, uint64_t lba, uint16_t count,
                         uint32_t bytes, bool write)
{
    struct cmd_header *h = &p->cmd_list[0];
    struct cmd_table *t = p->cmd_table;

    memset(t, 0, sizeof(*t));
    uint8_t *f = t->cfis;
    f[0] = FIS_TYPE_H2D;
    f[1] = 0x80;
    f[2] = ata_cmd;
    f[4] = lba & 0xFF;
    f[5] = (lba >> 8) & 0xFF;
    f[6] = (lba >> 16) & 0xFF;
    f[7] = 0x40;
    f[8] = (lba >> 24) & 0xFF;
    f[9] = (lba >> 32) & 0xFF;
    f[10] = (lba >> 40) & 0xFF;
    f[12] = count & 0xFF;
    f[13] = (count >> 8) & 0xFF;

    t->prdt[0].dba = (uint32_t)p->dma_phys;
    t->prdt[0].dbau = (uint32_t)(p->dma_phys >> 32);
    t->prdt[0].dbc = (bytes - 1) & 0x3FFFFF;

    h->flags = write ? (5 | (1 << 6)) : 5;
    h->prdtl = 1;
    h->prdbc = 0;

    if (!wait_clear(p->regs, PX_TFD, TFD_BSY | TFD_DRQ, 1000000))
        return false;

    wr(p->regs, PX_IS, 0xFFFFFFFF);
    wr(p->regs, PX_CI, 1);

    uint64_t end = time_now_us() + 2000000;
    while (rd(p->regs, PX_CI) & 1) {
        if (rd(p->regs, PX_TFD) & TFD_ERR)
            return false;
        if (time_now_us() > end)
            return false;
        if (interrupts_enabled() && sched_enabled())
            thread_yield();
        else
            pause();
    }
    bool ok = !(rd(p->regs, PX_TFD) & TFD_ERR);
    wr(p->regs, PX_IS, 0xFFFFFFFF);
    wr(abar, HBA_IS, 0xFFFFFFFF);
    return ok;
}

static bool ahci_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = dev->priv;
    uint8_t *out = buf;
    mutex_lock(&p->lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS_PER_CMD ? MAX_SECTORS_PER_CMD : count;
        if (!port_command(p, ATA_READ_DMA_EXT, lba, (uint16_t)n, n * 512, false)) {
            mutex_unlock(&p->lock);
            log_error("ahci", "porta %u: erro lendo LBA %lu", p->index, lba);
            return false;
        }
        memcpy(out, p->dma_buf, n * 512);
        out += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return true;
}

static bool ahci_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = dev->priv;
    const uint8_t *in = buf;
    mutex_lock(&p->lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS_PER_CMD ? MAX_SECTORS_PER_CMD : count;
        memcpy(p->dma_buf, in, n * 512);
        if (!port_command(p, ATA_WRITE_DMA_EXT, lba, (uint16_t)n, n * 512, true)) {
            mutex_unlock(&p->lock);
            log_error("ahci", "porta %u: erro escrevendo LBA %lu", p->index, lba);
            return false;
        }
        in += n * 512;
        lba += n;
        count -= n;
    }
    port_command(p, ATA_FLUSH_CACHE_EXT, 0, 0, 512, false);
    mutex_unlock(&p->lock);
    return true;
}

static bool port_identify(struct ahci_port *p)
{
    if (!port_command(p, ATA_IDENTIFY, 0, 1, 512, false))
        return false;
    uint16_t *id = (uint16_t *)p->dma_buf;

    uint64_t lba48;
    memcpy(&lba48, &id[100], 8);
    uint32_t lba28;
    memcpy(&lba28, &id[60], 4);
    p->sectors = lba48 ? lba48 : lba28;

    for (int i = 0; i < 20; i++) {
        p->model[i * 2] = (char)(id[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    p->model[40] = '\0';
    for (int i = 39; i >= 0 && p->model[i] == ' '; i--)
        p->model[i] = '\0';
    return true;
}

static void port_init(uint32_t index)
{
    struct ahci_port *p = &ports[port_count];
    p->index = index;
    p->regs = abar + HBA_PORTS + index * PORT_SIZE;
    ksnprintf(p->lock_name, sizeof(p->lock_name), "ahci%u", index);
    mutex_init(&p->lock, p->lock_name);

    port_stop(p);

    p->mem_phys = pmm_alloc_page();
    uint8_t *mem = phys_to_virt(p->mem_phys);
    memset(mem, 0, PAGE_SIZE);
    p->cmd_list = (struct cmd_header *)mem;
    p->cmd_table = (struct cmd_table *)(mem + 0x800);
    p->cmd_list[0].ctba = (uint32_t)(p->mem_phys + 0x800);
    p->cmd_list[0].ctbau = (uint32_t)((p->mem_phys + 0x800) >> 32);

    wr(p->regs, PX_CLB, (uint32_t)p->mem_phys);
    wr(p->regs, PX_CLBU, (uint32_t)(p->mem_phys >> 32));
    wr(p->regs, PX_FB, (uint32_t)(p->mem_phys + 0x400));
    wr(p->regs, PX_FBU, (uint32_t)((p->mem_phys + 0x400) >> 32));
    wr(p->regs, PX_SERR, 0xFFFFFFFF);
    wr(p->regs, PX_IE, 0);

    p->dma_phys = pmm_alloc_pages(DMA_PAGES);
    p->dma_buf = phys_to_virt(p->dma_phys);

    port_start(p);

    if (!port_identify(p)) {
        log_warn("ahci", "porta %u: IDENTIFY falhou", index);
        port_stop(p);
        return;
    }

    char name[BLOCK_NAME_MAX];
    ksnprintf(name, sizeof(name), "sd%c", 'a' + port_count);
    struct blockdev *dev = blockdev_register(name, 512, p->sectors, ahci_read, ahci_write, p);
    if (!dev)
        return;
    log_info("ahci", "%s: porta %u, '%s', %lu setores (%lu MiB)", name, index, p->model,
             p->sectors, p->sectors * 512 / MiB(1));
    port_count++;
    blockdev_scan_partitions(dev);
}

void ahci_init(void)
{
    const struct pci_device *dev = pci_find_class(0x01, 0x06, 0);
    if (!dev) {
        log_warn("ahci", "nenhum controlador AHCI no barramento PCI");
        return;
    }
    pci_enable_bus_master(dev);

    uint64_t abar_phys = dev->bar[5] & ~0xFull;
    abar = vmm_map_mmio(abar_phys, 0x1100);

    wr(abar, HBA_GHC, (rd(abar, HBA_GHC) | GHC_AE) & ~GHC_IE);
    uint32_t cap = rd(abar, HBA_CAP);
    uint32_t pi = rd(abar, HBA_PI);
    log_info("ahci", "controlador %04x:%04x em %02x:%02x.%u, ABAR %08lx, %u porta(s), %s",
             dev->vendor, dev->device, dev->bus, dev->slot, dev->func, abar_phys,
             (cap & 0x1F) + 1, (cap & (1u << 31)) ? "DMA 64 bits" : "DMA 32 bits");

    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1u << i)))
            continue;
        volatile uint8_t *regs = abar + HBA_PORTS + i * PORT_SIZE;
        uint32_t ssts = rd(regs, PX_SSTS);
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
            continue;
        if (rd(regs, PX_SIG) != SIG_SATA) {
            log_info("ahci", "porta %u: dispositivo nao-disco (sig %08x), ignorado", i, rd(regs, PX_SIG));
            continue;
        }
        port_init(i);
    }
}
