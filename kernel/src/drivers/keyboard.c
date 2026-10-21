#include <drivers/keyboard.h>
#include <drivers/input.h>
#include <drivers/pic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <drivers/conin.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL (1 << 0)

#define SC_RELEASE   0x80
#define SC_EXTENDED  0xE0
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CTRL      0x1D
#define SC_ALT       0x38
#define SC_CAPSLOCK  0x3A

static const char keymap[128] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0', [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*', [0x39] = ' ',
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.',
    [0x73] = '/', [0x7E] = '.',
};

static const char keymap_shift[128] = {
    [0x01] = 0x1B, [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(',
    [0x0B] = ')', [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G',
    [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*', [0x39] = ' ',
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.',
    [0x73] = '?', [0x7E] = '.',
};

#define BUF_SIZE 256
static char buf[BUF_SIZE];
static volatile uint32_t buf_head, buf_tail;

static uint8_t modifiers;
static bool extended_pending;
static spinlock_t kbd_lock = SPINLOCK_INIT;

static void push_char(char c)
{
    uint32_t next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail)
        return;
    buf[buf_head] = c;
    buf_head = next;
    conin_kick();
}

static const char *extended_sequence(uint8_t code)
{
    switch (code) {
    case 0x48: return "\x1b[A";
    case 0x50: return "\x1b[B";
    case 0x4D: return "\x1b[C";
    case 0x4B: return "\x1b[D";
    case 0x47: return "\x1b[H";
    case 0x4F: return "\x1b[F";
    case 0x49: return "\x1b[5~";
    case 0x51: return "\x1b[6~";
    case 0x52: return "\x1b[2~";
    case 0x53: return "\x1b[3~";
    case 0x1C: return "\n";
    case 0x35: return "/";
    default:   return NULL;
    }
}

static void feed(uint8_t sc)
{
    if (sc == SC_EXTENDED) {
        extended_pending = true;
        return;
    }
    if (extended_pending) {
        extended_pending = false;
        bool up = sc & SC_RELEASE;
        uint8_t ext = sc & 0x7F;
        if (ext == SC_CTRL) {
            if (up) modifiers &= ~KBD_MOD_CTRL; else modifiers |= KBD_MOD_CTRL;
        } else if (ext == SC_ALT) {
            if (up) modifiers &= ~KBD_MOD_ALT; else modifiers |= KBD_MOD_ALT;
        }
        const char *seq = extended_sequence(ext);
        bool grabbed = input_post_key((uint8_t)ext, 0, modifiers | KBD_MOD_EXTENDED, !up);
        if (!seq || grabbed)
            return;
        if (up)
            return;
        while (*seq)
            push_char(*seq++);
        return;
    }

    bool released = sc & SC_RELEASE;
    uint8_t code = sc & 0x7F;

    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        if (released) modifiers &= ~KBD_MOD_SHIFT; else modifiers |= KBD_MOD_SHIFT;
        input_post_key(code, 0, modifiers, !released);
        return;
    case SC_CTRL:
        if (released) modifiers &= ~KBD_MOD_CTRL; else modifiers |= KBD_MOD_CTRL;
        input_post_key(code, 0, modifiers, !released);
        return;
    case SC_ALT:
        if (released) modifiers &= ~KBD_MOD_ALT; else modifiers |= KBD_MOD_ALT;
        input_post_key(code, 0, modifiers, !released);
        return;
    case SC_CAPSLOCK:
        if (!released) modifiers ^= KBD_MOD_CAPS;
        input_post_key(code, 0, modifiers, !released);
        return;
    default:
        break;
    }

    bool shift = modifiers & KBD_MOD_SHIFT;
    char c = shift ? keymap_shift[code] : keymap[code];

    if ((modifiers & KBD_MOD_CAPS) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;

    if ((modifiers & KBD_MOD_CTRL) && c >= 'a' && c <= 'z')
        c = c - 'a' + 1;

    if (input_post_key(code, c, modifiers, !released))
        return;

    if (released || !c)
        return;

    push_char(c);
}

static void keyboard_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    uint8_t sc = inb(PS2_DATA);
    spin_lock(&kbd_lock);
    feed(sc);
    spin_unlock(&kbd_lock);
}

void keyboard_feed(uint8_t scancode)
{
    uint64_t flags = spin_lock_irqsave(&kbd_lock);
    feed(scancode);
    spin_unlock_irqrestore(&kbd_lock, flags);
}

void keyboard_init(void)
{
    if (inb(PS2_STATUS) == 0xFF) {
        log_info("kbd", "sem controlador PS/2 (i8042); o teclado vem do USB, se houver");
        return;
    }
    int guard = 64;
    while ((inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) && guard--)
        inb(PS2_DATA);

    buf_head = buf_tail = 0;
    modifiers = 0;
    extended_pending = false;

    interrupts_register(VECTOR_IRQ_BASE + IRQ_KEYBOARD, keyboard_irq, NULL);
    irq_enable(IRQ_KEYBOARD);

    log_info("kbd", "teclado PS/2 na IRQ1 (scancode set 1, layout US)");
}

int keyboard_getchar(void)
{
    uint64_t flags = irq_save();
    int c = -1;
    if (buf_tail != buf_head) {
        c = (unsigned char)buf[buf_tail];
        buf_tail = (buf_tail + 1) % BUF_SIZE;
    }
    irq_restore(flags);
    return c;
}

uint8_t keyboard_modifiers(void)
{
    return modifiers;
}
