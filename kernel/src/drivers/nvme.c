#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <fs/block.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/memory.h>
#include <lib/string.h>
#include <lib/printf.h>

#define REG_CAP      0x00
#define REG_CC       0x14
#define REG_CSTS     0x1C
#define REG_AQA      0x24
#define REG_ASQ      0x28
#define REG_ACQ      0x30
#define REG_DB       0x1000

#define CC_EN        (1u << 0)
#define CC_IOSQES    (6u << 16)
#define CC_IOCQES    (4u << 20)
#define CSTS_RDY     (1u << 0)
#define CSTS_CFS     (1u << 1)

#define OP_FLUSH     0x00
#define OP_WRITE     0x01
#define OP_READ      0x02
#define OP_CREATE_SQ 0x01
#define OP_CREATE_CQ 0x05
#define OP_IDENTIFY  0x06

#define ADMIN_SIZE   16
#define IO_SIZE      32
#define DMA_PAGES    16
#define MAX_SECTORS  ((DMA_PAGES * PAGE_SIZE) / 512)

struct nvme_cmd {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} PACKED;

struct nvme_cpl {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    uint16_t status;
} PACKED;

struct nvme_queue {
    struct nvme_cmd *sq;
    volatile struct nvme_cpl *cq;
    uint64_t sq_phys;
    uint64_t cq_phys;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    uint16_t size;
    uint16_t tail;
    uint16_t head;
    uint16_t cid;
    uint8_t phase;
};

static volatile uint8_t *regs;
static struct nvme_queue admin, io;
static struct mutex lock;
static spinlock_t irq_lock = SPINLOCK_INIT;
static struct waitqueue wq = WAITQUEUE_INIT;
static bool irq_on;
static uint8_t *dma;
static uint64_t dma_phys;
static uint64_t *prp_list;
static uint64_t prp_list_phys;
static uint64_t sectors;

static inline uint32_t rd32(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static inline uint64_t rd64(uint32_t off)
{
    return (uint64_t)rd32(off) | ((uint64_t)rd32(off + 4) << 32);
}

static inline void wr32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
}

static inline void wr64(uint32_t off, uint64_t v)
{
    wr32(off, (uint32_t)v);
    wr32(off + 4, (uint32_t)(v >> 32));
}

static bool wait_ready(bool ready, uint64_t timeout_ms)
{
    uint64_t end = time_now_us() + timeout_ms * 1000;
    while (((rd32(REG_CSTS) & CSTS_RDY) != 0) != ready) {
        if (rd32(REG_CSTS) & CSTS_CFS)
            return false;
        if (time_now_us() > end)
            return false;
        pause();
    }
    return true;
}

static bool queue_alloc(struct nvme_queue *q, uint16_t qid, uint16_t size, uint32_t stride)
{
    memset(q, 0, sizeof(*q));
    q->sq_phys = pmm_alloc_page();
    q->cq_phys = pmm_alloc_page();
    if (!q->sq_phys || !q->cq_phys)
        return false;
    q->sq = phys_to_virt(q->sq_phys);
    q->cq = phys_to_virt(q->cq_phys);
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE);
    q->size = size;
    q->phase = 1;
    q->sq_db = (volatile uint32_t *)(regs + REG_DB + (2u * qid) * stride);
    q->cq_db = (volatile uint32_t *)(regs + REG_DB + (2u * qid + 1) * stride);
    return true;
}

static uint16_t submit(struct nvme_queue *q, struct nvme_cmd *cmd)
{
    uint16_t cid = q->cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);
    q->sq[q->tail] = *cmd;
    q->tail = (uint16_t)((q->tail + 1) % q->size);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->sq_db = q->tail;
    return cid;
}

static bool cq_ready(struct nvme_queue *q)
{
    return (q->cq[q->head].status & 1) == q->phase;
}

static bool reap(struct nvme_queue *q, uint16_t cid, uint16_t *status)
{
    while (cq_ready(q)) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        volatile struct nvme_cpl *c = &q->cq[q->head];
        uint16_t got = c->cid;
        uint16_t st = (uint16_t)(c->status >> 1);
        q->head = (uint16_t)((q->head + 1) % q->size);
        if (q->head == 0)
            q->phase ^= 1;
        *q->cq_db = q->head;
        if (got == cid) {
            *status = st;
            return true;
        }
    }
    return false;
}

static bool run(struct nvme_queue *q, struct nvme_cmd *cmd, bool can_sleep)
{
    uint16_t cid = submit(q, cmd);
    uint16_t status = 0;
    uint64_t end = time_now_us() + 5000000;
    for (;;) {
        if (can_sleep) {
            uint64_t flags = spin_lock_irqsave(&irq_lock);
            if (!cq_ready(q))
                wait_sleep_timeout(&wq, &irq_lock, 10);
            spin_unlock_irqrestore(&irq_lock, flags);
        }
        if (reap(q, cid, &status))
            return status == 0;
        if (time_now_us() > end)
            return false;
        if (!can_sleep) {
            if (interrupts_enabled() && sched_enabled())
                thread_yield();
            else
                pause();
        }
    }
}

static void nvme_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    spin_lock(&irq_lock);
    wake_up_all(&wq);
    spin_unlock(&irq_lock);
}

static bool admin_cmd(uint8_t op, uint32_t nsid, uint64_t prp1, uint32_t cdw10, uint32_t cdw11)
{
    struct nvme_cmd c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = op;
    c.nsid = nsid;
    c.prp1 = prp1;
    c.cdw10 = cdw10;
    c.cdw11 = cdw11;
    return run(&admin, &c, false);
}

static bool io_cmd(uint8_t op, uint64_t lba, uint32_t count)
{
    struct nvme_cmd c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = op;
    c.nsid = 1;
    if (op != OP_FLUSH) {
        uint32_t bytes = count * 512;
        uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        c.prp1 = dma_phys;
        if (pages == 2)
            c.prp2 = dma_phys + PAGE_SIZE;
        else if (pages > 2)
            c.prp2 = prp_list_phys;
        c.cdw10 = (uint32_t)lba;
        c.cdw11 = (uint32_t)(lba >> 32);
        c.cdw12 = count - 1;
    }
    return run(&io, &c, irq_on && interrupts_enabled() && sched_enabled());
}

static bool nvme_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    uint8_t *out = buf;
    mutex_lock(&lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        if (!io_cmd(OP_READ, lba, n)) {
            mutex_unlock(&lock);
            log_error("nvme", "%s: erro lendo LBA %lu", dev->name, lba);
            return false;
        }
        memcpy(out, dma, n * 512);
        out += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&lock);
    return true;
}

static bool nvme_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    const uint8_t *in = buf;
    mutex_lock(&lock);
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        memcpy(dma, in, n * 512);
        if (!io_cmd(OP_WRITE, lba, n)) {
            mutex_unlock(&lock);
            log_error("nvme", "%s: erro escrevendo LBA %lu", dev->name, lba);
            return false;
        }
        in += n * 512;
        lba += n;
        count -= n;
    }
    io_cmd(OP_FLUSH, 0, 0);
    mutex_unlock(&lock);
    return true;
}

static void trim(char *s, size_t len)
{
    s[len] = 0;
    for (int i = (int)len - 1; i >= 0 && (s[i] == ' ' || s[i] == 0); i--)
        s[i] = 0;
}

bool nvme_init(void)
{
    const struct pci_device *pci = pci_find_class(0x01, 0x08, 0);
    if (!pci)
        return false;
    uint64_t base = pci_bar_address(pci, 0);
    if (!base) {
        log_warn("nvme", "controlador sem BAR de memoria");
        return false;
    }
    pci_enable_bus_master(pci);
    regs = vmm_map_mmio(base, 0x2000);

    uint64_t cap = rd64(REG_CAP);
    uint32_t stride = 4u << ((cap >> 32) & 0xF);
    uint64_t timeout = ((cap >> 24) & 0xFF) * 500 + 500;
    uint16_t io_size = IO_SIZE;
    if ((cap & 0xFFFF) + 1 < io_size)
        io_size = (uint16_t)((cap & 0xFFFF) + 1);

    wr32(REG_CC, 0);
    if (!wait_ready(false, timeout)) {
        log_warn("nvme", "controlador nao desligou");
        return false;
    }
    if (!queue_alloc(&admin, 0, ADMIN_SIZE, stride) || !queue_alloc(&io, 1, io_size, stride))
        return false;
    wr32(REG_AQA, ((uint32_t)(ADMIN_SIZE - 1) << 16) | (ADMIN_SIZE - 1));
    wr64(REG_ASQ, admin.sq_phys);
    wr64(REG_ACQ, admin.cq_phys);
    wr32(REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (!wait_ready(true, timeout)) {
        log_warn("nvme", "controlador nao ligou (CSTS %08x)", rd32(REG_CSTS));
        return false;
    }

    uint64_t id_phys = pmm_alloc_page();
    dma_phys = pmm_alloc_pages(DMA_PAGES);
    prp_list_phys = pmm_alloc_page();
    if (!id_phys || !dma_phys || !prp_list_phys)
        return false;
    uint8_t *id = phys_to_virt(id_phys);
    dma = phys_to_virt(dma_phys);
    prp_list = phys_to_virt(prp_list_phys);
    memset(prp_list, 0, PAGE_SIZE);
    for (uint32_t i = 1; i < DMA_PAGES; i++)
        prp_list[i - 1] = dma_phys + (uint64_t)i * PAGE_SIZE;

    char model[41];
    if (!admin_cmd(OP_IDENTIFY, 0, id_phys, 1, 0)) {
        log_warn("nvme", "Identify do controlador falhou");
        return false;
    }
    memcpy(model, id + 24, 40);
    trim(model, 40);
    if (!admin_cmd(OP_IDENTIFY, 1, id_phys, 0, 0)) {
        log_warn("nvme", "Identify do namespace 1 falhou");
        return false;
    }
    memcpy(&sectors, id, 8);
    uint8_t flbas = id[26] & 0xF;
    uint32_t lbaf;
    memcpy(&lbaf, id + 128 + flbas * 4, 4);
    uint32_t lbads = (lbaf >> 16) & 0xFF;
    if (lbads != 9) {
        log_warn("nvme", "namespace 1 com blocos de %u bytes; so 512 e suportado", 1u << lbads);
        return false;
    }

    int vector = interrupts_alloc_vector(nvme_irq, NULL, "nvme");
    uint32_t cpu = irq_next_cpu();
    irq_on = vector >= 0 && pci_enable_msix(pci, 0, (uint8_t)vector, cpu);
    if (!irq_on && vector >= 0)
        interrupts_unregister((uint8_t)vector);
    if (!admin_cmd(OP_CREATE_CQ, 0, io.cq_phys, ((uint32_t)(io_size - 1) << 16) | 1, irq_on ? 3 : 1) ||
        !admin_cmd(OP_CREATE_SQ, 0, io.sq_phys, ((uint32_t)(io_size - 1) << 16) | 1, (1u << 16) | 1)) {
        log_warn("nvme", "criar as filas de I/O falhou");
        return false;
    }
    mutex_init(&lock, "nvme0");

    char how[48];
    if (irq_on)
        ksnprintf(how, sizeof(how), "MSI-X no vetor %d, CPU %u", vector, cpu);
    else
        ksnprintf(how, sizeof(how), "sem MSI-X, espera ativa");
    struct blockdev *dev = blockdev_register("nvme0n1", 512, sectors, nvme_read, nvme_write, NULL);
    if (!dev)
        return false;
    strlcpy(dev->model, model, sizeof(dev->model));
    log_info("nvme", "nvme0n1: %04x:%04x em %02x:%02x.%u, '%s', versao %u.%u, fila de %u, %lu setores (%lu MiB), %s",
             pci->vendor, pci->device, pci->bus, pci->slot, pci->func, model, rd32(0x08) >> 16,
             (rd32(0x08) >> 8) & 0xFF, io_size, sectors, sectors * 512 / MiB(1), how);
    blockdev_scan_partitions(dev);
    return true;
}
