#include <drivers/serial.h>
#include <arch/x86_64/cpu.h>

#define REG_DATA        0
#define REG_IER         1   
#define REG_FCR         2   
#define REG_LCR         3   
#define REG_MCR         4   
#define REG_LSR         5   
#define REG_SCRATCH     7

#define LST_DATA_READY  (1 << 0)
#define LST_THR_EMPTY   (1 << 5)

static bool present = false;

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

static inline void wait_tx(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(SERIAL_COM1 + REG_LSR) & LST_THR_EMPTY)
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
    if (!(inb(SERIAL_COM1 + REG_LSR) & LST_DATA_READY))
        return -1;
    return inb(SERIAL_COM1 + REG_DATA);
}