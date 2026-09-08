/*
 * serial.c - driver da UART 16550 (COM1).
 */
#include <drivers/serial.h>
#include <arch/x86_64/cpu.h>

/* Offsets dos registradores a partir da base da porta. */
#define REG_DATA        0   /* RBR/THR (DLAB=0) | DLL (DLAB=1) */
#define REG_IER         1   /* Interrupt Enable | DLM (DLAB=1) */
#define REG_FCR         2   /* FIFO Control (escrita) / IIR (leitura) */
#define REG_LCR         3   /* Line Control */
#define REG_MCR         4   /* Modem Control */
#define REG_LSR         5   /* Line Status */
#define REG_SCRATCH     7

#define LSR_DATA_READY  (1 << 0)
#define LSR_THR_EMPTY   (1 << 5)

static bool present = false;

void serial_init(void)
{
    const uint16_t port = SERIAL_COM1;

    outb(port + REG_IER, 0x00);        /* desliga interrupções da UART */
    outb(port + REG_LCR, 0x80);        /* DLAB=1 para programar o divisor */
    outb(port + REG_DATA, 0x01);       /* divisor = 1 -> 115200 baud */
    outb(port + REG_IER, 0x00);
    outb(port + REG_LCR, 0x03);        /* 8 bits, sem paridade, 1 stop bit */
    outb(port + REG_FCR, 0xC7);        /* FIFO on, limpa, threshold 14 bytes */
    outb(port + REG_MCR, 0x0B);        /* DTR | RTS | OUT2 */

    /* Teste de loopback: se o chip devolve o byte, ele existe. */
    outb(port + REG_MCR, 0x1E);        /* modo loopback */
    outb(port + REG_DATA, 0xAE);
    if (inb(port + REG_DATA) != 0xAE) {
        present = false;
        return;
    }
    outb(port + REG_MCR, 0x0F);        /* sai do loopback, DTR|RTS|OUT1|OUT2 */
    present = true;
}

bool serial_available(void)
{
    return present;
}

static inline void wait_tx(void)
{
    /* Espera o Transmitter Holding Register esvaziar (com limite para não
     * travar caso o hardware esteja esquisito). */
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
    if (!(inb(SERIAL_COM1 + REG_LSR) & LSR_DATA_READY))
        return -1;
    return inb(SERIAL_COM1 + REG_DATA);
}
