#include <drivers/serial.h>
#include <drivers/conin.h>
#include <drivers/pic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>

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
    while (inb(SERIAL_COM1 + REG_LSR) & LSR_DATA_READY) {
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
