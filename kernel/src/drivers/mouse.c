#include <drivers/mouse.h>
#include <drivers/input.h>
#include <drivers/pic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/spinlock.h>
#include <kernel/log.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_STATUS_OUTPUT_FULL (1 << 0)
#define PS2_STATUS_INPUT_FULL  (1 << 1)
#define PS2_STATUS_AUX_DATA    (1 << 5)

#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_TO_AUX       0xD4

#define PS2_CONFIG_AUX_IRQ   (1 << 1)
#define PS2_CONFIG_AUX_CLOCK (1 << 5)

#define AUX_SET_DEFAULTS  0xF6
#define AUX_ENABLE_REPORT 0xF4
#define AUX_GET_ID        0xF2
#define AUX_SET_RATE      0xF3
#define AUX_ACK           0xFA

#define QUEUE_SIZE 64
#define IO_TIMEOUT 100000

static spinlock_t lock = SPINLOCK_INIT;
static struct mouse_packet queue[QUEUE_SIZE];
static uint32_t queue_head, queue_tail;

static uint8_t packet[4];
static uint8_t packet_index;
static uint8_t last_buttons;
static uint8_t packet_size = 3;
static bool wheel;
static bool available;
static bool usb_mouse;
static uint64_t packets;
static uint64_t resyncs;

static bool wait_writable(void)
{
    for (int i = 0; i < IO_TIMEOUT; i++)
        if (!(inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL))
            return true;
    return false;
}

static bool wait_readable(void)
{
    for (int i = 0; i < IO_TIMEOUT; i++)
        if (inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL)
            return true;
    return false;
}

static void controller_command(uint8_t cmd)
{
    if (wait_writable())
        outb(PS2_CMD, cmd);
}

static int read_data(void)
{
    if (!wait_readable())
        return -1;
    return inb(PS2_DATA);
}

static int aux_command(uint8_t cmd)
{
    controller_command(PS2_CMD_TO_AUX);
    if (!wait_writable())
        return -1;
    outb(PS2_DATA, cmd);
    return read_data();
}

static bool set_sample_rate(uint8_t rate)
{
    if (aux_command(AUX_SET_RATE) != AUX_ACK)
        return false;
    return aux_command(rate) == AUX_ACK;
}

static void queue_push(const struct mouse_packet *p)
{
    uint32_t next = (queue_head + 1) % QUEUE_SIZE;
    if (next == queue_tail)
        queue_tail = (queue_tail + 1) % QUEUE_SIZE;
    queue[queue_head] = *p;
    queue_head = next;
}

static void deliver(const struct mouse_packet *p)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    packets++;
    queue_push(p);
    bool changed = p->buttons != last_buttons;
    last_buttons = p->buttons;
    spin_unlock_irqrestore(&lock, flags);

    if (p->dx || p->dy || p->dz)
        input_post_mouse_move(p->dx, p->dy, p->dz);
    if (changed)
        input_post_mouse_button(p->buttons);
}

static void decode_packet(void)
{
    uint8_t flags = packet[0];
    if (flags & 0xC0)
        return;

    struct mouse_packet p;
    p.buttons = flags & 0x07;
    p.dx = (int32_t)packet[1] - ((flags & 0x10) ? 256 : 0);
    p.dy = ((flags & 0x20) ? 256 : 0) - (int32_t)packet[2];
    p.dz = 0;
    if (packet_size == 4) {
        int8_t z = (int8_t)(packet[3] & 0x0F);
        if (z & 0x08)
            z |= (int8_t)0xF0;
        p.dz = z;
    }
    deliver(&p);
}

void mouse_inject(int32_t dx, int32_t dy, int32_t dz, uint8_t buttons)
{
    struct mouse_packet p = { dx, dy, dz, (uint8_t)(buttons & 0x07) };
    usb_mouse = true;
    deliver(&p);
}

static void mouse_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;

    uint8_t status = inb(PS2_STATUS);
    if (!(status & PS2_STATUS_OUTPUT_FULL))
        return;
    uint8_t data = inb(PS2_DATA);
    if (!(status & PS2_STATUS_AUX_DATA))
        return;

    if (packet_index == 0 && !(data & 0x08)) {
        resyncs++;
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < packet_size)
        return;
    packet_index = 0;
    decode_packet();
}

void mouse_init(void)
{
    int guard = 64;
    while ((inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) && guard--)
        inb(PS2_DATA);

    controller_command(PS2_CMD_ENABLE_AUX);

    controller_command(PS2_CMD_READ_CONFIG);
    int config = read_data();
    if (config < 0) {
        log_warn("mouse", "controlador 8042 nao respondeu; mouse desativado");
        return;
    }
    config |= PS2_CONFIG_AUX_IRQ;
    config &= ~PS2_CONFIG_AUX_CLOCK;
    controller_command(PS2_CMD_WRITE_CONFIG);
    if (wait_writable())
        outb(PS2_DATA, (uint8_t)config);

    if (aux_command(AUX_SET_DEFAULTS) != AUX_ACK) {
        log_warn("mouse", "dispositivo auxiliar ausente");
        return;
    }

    if (set_sample_rate(200) && set_sample_rate(100) && set_sample_rate(80) &&
        aux_command(AUX_GET_ID) == AUX_ACK) {
        int id = read_data();
        if (id == 3) {
            wheel = true;
            packet_size = 4;
        }
    }

    if (aux_command(AUX_ENABLE_REPORT) != AUX_ACK) {
        log_warn("mouse", "dispositivo nao aceitou 0xF4 (enable)");
        return;
    }

    packet_index = 0;
    queue_head = queue_tail = 0;
    available = true;

    interrupts_register(VECTOR_IRQ_BASE + IRQ_MOUSE, mouse_irq, NULL);
    irq_enable(IRQ_MOUSE);

    log_info("mouse", "mouse PS/2 na IRQ12, pacotes de %u bytes%s",
             packet_size, wheel ? " (roda ativa)" : "");
}

bool mouse_available(void)
{
    return available || usb_mouse;
}

bool mouse_has_wheel(void)
{
    return wheel;
}

bool mouse_poll(struct mouse_packet *out)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    bool got = queue_tail != queue_head;
    if (got) {
        *out = queue[queue_tail];
        queue_tail = (queue_tail + 1) % QUEUE_SIZE;
    }
    spin_unlock_irqrestore(&lock, flags);
    return got;
}

uint64_t mouse_packet_count(void)
{
    return packets;
}

uint64_t mouse_resync_count(void)
{
    return resyncs;
}
