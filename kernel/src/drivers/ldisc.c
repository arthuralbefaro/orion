#include <drivers/ldisc.h>
#include <kernel/sched.h>
#include <lib/string.h>

#define POLL_MS 2

static void ready_push(struct line_discipline *l, const char *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (l->head + 1) % LDISC_READY_MAX;
        if (next == l->tail)
            return;
        l->ready[l->head] = src[i];
        l->head = next;
    }
}

void ldisc_init(struct line_discipline *l, ldisc_echo_fn echo, void *ctx)
{
    memset(l, 0, sizeof(*l));
    spinlock_init(&l->lock);
    l->echo = echo;
    l->ctx = ctx;
}

void ldisc_input(struct line_discipline *l, char c)
{
    char out[4];
    uint32_t out_len = 0;

    if (c == '\r')
        c = '\n';

    uint64_t flags = spin_lock_irqsave(&l->lock);
    switch (c) {
    case '\b':
    case 0x7F:
        if (l->line_len > 0) {
            l->line_len--;
            memcpy(out, "\b \b", 3);
            out_len = 3;
        }
        break;
    case 0x03:
        l->line_len = 0;
        memcpy(out, "^C\n", 3);
        out_len = 3;
        break;
    case 0x04:
        if (l->line_len > 0) {
            ready_push(l, l->line, l->line_len);
            l->line_len = 0;
        } else {
            l->eof = true;
        }
        break;
    case '\n':
        l->line[l->line_len++] = '\n';
        ready_push(l, l->line, l->line_len);
        l->line_len = 0;
        out[0] = '\n';
        out_len = 1;
        break;
    default:
        if (c >= 0x20 && c < 0x7F && l->line_len < LDISC_LINE_MAX - 1) {
            l->line[l->line_len++] = c;
            out[0] = c;
            out_len = 1;
        }
        break;
    }
    spin_unlock_irqrestore(&l->lock, flags);

    if (out_len && l->echo)
        l->echo(l->ctx, out, out_len);
}

void ldisc_write(struct line_discipline *l, const void *buf, uint64_t len)
{
    const char *s = buf;
    for (uint64_t i = 0; i < len; i++)
        ldisc_input(l, s[i]);
}

uint32_t ldisc_available(struct line_discipline *l)
{
    uint64_t flags = spin_lock_irqsave(&l->lock);
    uint32_t n = (l->head - l->tail + LDISC_READY_MAX) % LDISC_READY_MAX;
    spin_unlock_irqrestore(&l->lock, flags);
    return n;
}

void ldisc_hangup(struct line_discipline *l)
{
    uint64_t flags = spin_lock_irqsave(&l->lock);
    l->hangup = true;
    spin_unlock_irqrestore(&l->lock, flags);
}

int64_t ldisc_read(struct line_discipline *l, void *buf, uint64_t len)
{
    char *out = buf;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&l->lock);
        uint64_t n = 0;
        while (n < len && l->tail != l->head) {
            out[n++] = l->ready[l->tail];
            l->tail = (l->tail + 1) % LDISC_READY_MAX;
        }
        bool got = n > 0;
        bool at_end = false;
        if (!got) {
            if (l->eof) {
                l->eof = false;
                at_end = true;
            } else if (l->hangup) {
                at_end = true;
            }
        }
        spin_unlock_irqrestore(&l->lock, flags);

        if (got)
            return (int64_t)n;
        if (at_end)
            return 0;
        thread_sleep_ms(POLL_MS);
    }
}
