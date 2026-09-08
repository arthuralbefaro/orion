/*
 * pic.c - driver do 8259A.
 */
#include <drivers/pic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10  /* inicialização */
#define ICW1_ICW4 0x01  /* ICW4 será enviado */
#define ICW4_8086 0x01  /* modo 8086/88 */

#define OCW2_EOI  0x20
#define OCW3_READ_ISR 0x0B

void pic_init(void)
{
    /* ICW1: começa a sequência de inicialização (em cascata). */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vetor base de cada chip. */
    outb(PIC1_DATA, VECTOR_IRQ_BASE);      io_wait();
    outb(PIC2_DATA, VECTOR_IRQ_BASE + 8);  io_wait();

    /* ICW3: master tem o slave na linha IRQ2; slave tem identidade 2. */
    outb(PIC1_DATA, 1 << IRQ_CASCADE);     io_wait();
    outb(PIC2_DATA, IRQ_CASCADE);          io_wait();

    /* ICW4: modo 8086. */
    outb(PIC1_DATA, ICW4_8086);            io_wait();
    outb(PIC2_DATA, ICW4_8086);            io_wait();

    /* Mascara tudo; cada driver desmascara a sua IRQ. A cascata (IRQ2)
     * precisa ficar aberta para as IRQs 8-15 chegarem. */
    outb(PIC1_DATA, 0xFF & ~(1 << IRQ_CASCADE));
    outb(PIC2_DATA, 0xFF);

    log_info("pic", "8259 remapeado: IRQ0-15 -> vetores %u-%u",
             VECTOR_IRQ_BASE, VECTOR_IRQ_BASE + 15);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq & 7;
    outb(port, inb(port) | (1 << bit));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq & 7;
    outb(port, inb(port) & ~(1 << bit));
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, OCW2_EOI);
    outb(PIC1_CMD, OCW2_EOI);
}

uint16_t pic_get_mask(void)
{
    return (uint16_t)inb(PIC1_DATA) | ((uint16_t)inb(PIC2_DATA) << 8);
}

static uint16_t pic_read_isr(void)
{
    outb(PIC1_CMD, OCW3_READ_ISR);
    outb(PIC2_CMD, OCW3_READ_ISR);
    return (uint16_t)inb(PIC1_CMD) | ((uint16_t)inb(PIC2_CMD) << 8);
}

bool pic_is_spurious(uint8_t irq)
{
    if (irq != 7 && irq != 15)
        return false;

    uint16_t isr = pic_read_isr();
    if (isr & (1 << irq))
        return false; /* IRQ real */

    /* Espúria. Para IRQ15 o master ainda acha que o slave levantou IRQ2,
     * então precisa de EOI só no master. */
    if (irq == 15)
        outb(PIC1_CMD, OCW2_EOI);
    return true;
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}
