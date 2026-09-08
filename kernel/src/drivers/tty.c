#include <drivers/tty.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <lib/printf.h>

#define LINE_MAX 256

static spinlock_t lock = SPINLOCK_INIT;
static char line[LINE_MAX];
static uint64_t line_len;
static char ready[LINE_MAX];
static uint64_t ready_len, ready_pos;
static bool ready_valid;
static bool eof_pending;

void tty_init(void)
{
    line_len = 0;
    ready_valid = false;
}

void tty_input(char c)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    if (c == '\r')
        c = '\n';
    switch (c) {
    case '\b':
    case 0x7F:
        if (line_len > 0) {
            line_len--;
            spin_unlock_irqrestore(&lock, flags);
            kputs("\b \b");
            return;
        }
        break;
    case 0x03:
        line_len = 0;
        spin_unlock_irqrestore(&lock, flags);
        kputs("^C\n");
        return;
    case 0x04:
        if (!ready_valid) {
            eof_pending = true;
        }
        break;
    case '\n':
        if (!ready_valid) {
            line[line_len++] = '\n';
            for (uint64_t i = 0; i < line_len; i++)
                ready[i] = line[i];
            ready_len = line_len;
            ready_pos = 0;
            ready_valid = true;
            line_len = 0;
        }
        spin_unlock_irqrestore(&lock, flags);
        kputc('\n');
        return;
    default:
        if (c >= 0x20 && c < 0x7F && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            spin_unlock_irqrestore(&lock, flags);
            kputc(c);
            return;
        }
        break;
    }
    spin_unlock_irqrestore(&lock, flags);
}

void tty_write(const void *buf, uint64_t len)
{
    const char *s = buf;
    uint64_t flags;
    bool locked = kprint_lock(&flags);
    for (uint64_t i = 0; i < len; i++)
        kputc(s[i]);
    kprint_unlock(locked, flags);
}

int64_t tty_read(void *buf, uint64_t len)
{
    char *out = buf;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&lock);
        if (ready_valid) {
            uint64_t n = 0;
            while (n < len && ready_pos < ready_len)
                out[n++] = ready[ready_pos++];
            if (ready_pos >= ready_len)
                ready_valid = false;
            spin_unlock_irqrestore(&lock, flags);
            return (int64_t)n;
        }
        if (eof_pending) {
            eof_pending = false;
            spin_unlock_irqrestore(&lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&lock, flags);
        thread_sleep_ms(5);
    }
}
