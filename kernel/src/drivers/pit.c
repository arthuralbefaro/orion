/*
 * pit.c - driver do 8254.
 */
#include <drivers/pit.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <drivers/pic.h>
#include <kernel/log.h>

#define PIT_CH0_DATA 0x40
#define PIT_CH2_DATA 0x42
#define PIT_CMD      0x43
#define PIT_GATE     0x61   /* bit 0: gate do canal 2; bit 1: speaker; bit 5: OUT2 */

/* canal 0 | acesso lo/hi byte | modo 3 (onda quadrada) | binário */
#define PIT_CMD_CH0_SQUARE 0x36
/* canal 2 | acesso lo/hi byte | modo 0 (interrupt on terminal count) */
#define PIT_CMD_CH2_ONESHOT 0xB0

void pit_init(uint32_t hz)
{
    uint32_t divisor = PIT_BASE_FREQUENCY / hz;
    if (divisor < 1)
        divisor = 1;
    if (divisor > 65535)
        divisor = 65535;

    outb(PIT_CMD, PIT_CMD_CH0_SQUARE);
    outb(PIT_CH0_DATA, divisor & 0xFF);
    outb(PIT_CH0_DATA, (divisor >> 8) & 0xFF);

    irq_enable(IRQ_TIMER);
    log_info("pit", "canal 0 a %u Hz (divisor %u) na IRQ0", PIT_BASE_FREQUENCY / divisor, divisor);
}

void pit_channel2_wait_ms(uint32_t ms)
{
    uint32_t count = PIT_BASE_FREQUENCY * ms / 1000;
    if (count > 65535)
        count = 65535;

    /* Gate do canal 2 desligado, speaker desligado. */
    uint8_t gate = inb(PIT_GATE) & ~0x03;
    outb(PIT_GATE, gate);

    outb(PIT_CMD, PIT_CMD_CH2_ONESHOT);
    outb(PIT_CH2_DATA, count & 0xFF);
    outb(PIT_CH2_DATA, (count >> 8) & 0xFF);

    /* Liga o gate: a contagem começa. OUT2 vai a 0 e sobe no fim. */
    outb(PIT_GATE, gate | 0x01);
    while (!(inb(PIT_GATE) & 0x20))
        pause();
    outb(PIT_GATE, gate);
}
