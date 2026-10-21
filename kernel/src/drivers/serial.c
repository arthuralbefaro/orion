#include <drivers/serial.h>
#include <drivers/conin.h>
#include <drivers/pic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>

#define REG_DATA        0
#define REG_IER         1
#define REG_FCR         2
#define REG_LCR         3
#define REG_MCR         4
#define REG_LSR         5
#define REG_SCRATCH     7

#define LSR_DATA_READY  (1 << 0)
#define LSR_THR_EMPTY   (1 << 5)
#define IER_RX          (1 << 0)
#define IER_THRE        (1 << 1)
#define RX_SIZE         1024

static bool present = false;
static spinlock_t rx_lock = SPINLOCK_INIT;
static uint8_t rx[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static uint64_t rx_irqs;

void serial_init(void)
{
    const uint16_t port = SERIAL_COM1;

    outb(port + REG_IER, 0x00);
    outb(port + REG_LCR, 0x80);
    outb(port + REG_DATA, 0x01);
    outb(port + REG_IER, 0x00);
    outb(port + REG_LCR, 0x03);
    outb(port + REG_FCR, 0xC7);
    outb(port + REG_MCR, 0x0B);

    outb(port + REG_MCR, 0x1E);
    outb(port + REG_DATA, 0xAE);
    if (inb(port + REG_DATA) != 0xAE) {
        present = false;
        return;
    }
    outb(port + REG_MCR, 0x0F);
    present = true;
}

bool serial_available(void)
{
    return present;
}

static void drain_uart(void)
{
    for (uint32_t n = 0; n < RX_SIZE; n++) {
        uint8_t lsr = inb(SERIAL_COM1 + REG_LSR);
        if (lsr == 0xFF || !(lsr & LSR_DATA_READY))
            break;
        uint8_t c = inb(SERIAL_COM1 + REG_DATA);
        uint32_t next = (rx_head + 1) % RX_SIZE;
        if (next != rx_tail) {
            rx[rx_head] = c;
            rx_head = next;
        }
    }
}

static void serial_irq(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    rx_irqs++;
    spin_lock(&rx_lock);
    drain_uart();
    spin_unlock(&rx_lock);
    conin_kick();
}

void serial_enable_irq(void)
{
    if (!present)
        return;
    interrupts_register(VECTOR_IRQ_BASE + IRQ_COM1, serial_irq, NULL);
    irq_enable(IRQ_COM1);
    outb(SERIAL_COM1 + REG_IER, IER_RX);
    log_info("serial", "COM1 com IRQ%u de recepcao (anel de %u bytes)", IRQ_COM1, RX_SIZE);
}

static volatile uint64_t storm_fires;

static void storm_irq(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    storm_fires++;
}

int serial_storm_test(uint64_t *fires, uint64_t *ms)
{
    *fires = 0;
    *ms = 0;
    if (!present || irq_get_mode() != IRQ_MODE_IOAPIC)
        return -ENODEV;
    uint8_t vec = VECTOR_IRQ_BASE + IRQ_COM1;
    uint32_t gsi;
    bool low, level;
    storm_fires = 0;
    interrupts_register(vec, storm_irq, NULL);
    if (!irq_enable_isa_as(IRQ_COM1, false, true, &gsi, &low, &level) || !level) {
        interrupts_register(vec, serial_irq, NULL);
        irq_enable(IRQ_COM1);
        return -ENODEV;
    }
    uint64_t t0 = time_now_us();
    outb(SERIAL_COM1 + REG_IER, IER_RX | IER_THRE);
    while (!interrupts_storm_masked(vec) && time_now_us() < t0 + 3000000)
        pause();
    *ms = (time_now_us() - t0) / 1000;
    bool masked = interrupts_storm_masked(vec);
    outb(SERIAL_COM1 + REG_IER, IER_RX);
    uint64_t seen = storm_fires;
    time_sleep_ms(20);
    bool quiet = storm_fires == seen;
    *fires = storm_fires;
    interrupts_register(vec, serial_irq, NULL);
    irq_enable(IRQ_COM1);
    return masked && quiet && !interrupts_storm_masked(vec) ? 0 : -EIO;
}

uint64_t serial_rx_irqs(void)
{
    return rx_irqs;
}

static inline void wait_tx(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(SERIAL_COM1 + REG_LSR) & LSR_THR_EMPTY)
            return;
    }
}

void serial_putc(char c)
{
    if (!present)
        return;
    if (c == '\n') {
        wait_tx();
        outb(SERIAL_COM1 + REG_DATA, '\r');
    }
    wait_tx();
    outb(SERIAL_COM1 + REG_DATA, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

int serial_getc(void)
{
    if (!present)
        return -1;
    uint64_t flags = spin_lock_irqsave(&rx_lock);
    if (rx_tail == rx_head)
        drain_uart();
    int c = -1;
    if (rx_tail != rx_head) {
        c = rx[rx_tail];
        rx_tail = (rx_tail + 1) % RX_SIZE;
    }
    spin_unlock_irqrestore(&rx_lock, flags);
    return c;
}
