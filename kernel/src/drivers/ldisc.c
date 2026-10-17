#include <drivers/ldisc.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/syscall.h>
#include <kernel/signal.h>
#include <kernel/process.h>
#include <kernel/log.h>
#include <lib/string.h>

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
    waitqueue_init(&l->readers);
    l->echo = echo;
    l->ctx = ctx;
    l->canonical = true;
    l->echo_on = true;
}

void ldisc_set_mode(struct line_discipline *l, bool canonical, bool echo_on)
{
    uint64_t flags = spin_lock_irqsave(&l->lock);
    l->canonical = canonical;
    l->echo_on = echo_on;
    if (!canonical && l->line_len) {
        ready_push(l, l->line, l->line_len);
        l->line_len = 0;
    }
    spin_unlock_irqrestore(&l->lock, flags);
    wake_up_all(&l->readers);
}

void ldisc_get_mode(struct line_discipline *l, bool *canonical, bool *echo_on)
{
    *canonical = l->canonical;
    *echo_on = l->echo_on;
}

void ldisc_input(struct line_discipline *l, char c)
{
    char out[4];
    uint32_t out_len = 0;
    int32_t interrupt_pgid = 0;

    if (c == '\r')
        c = '\n';

    uint64_t flags = spin_lock_irqsave(&l->lock);
    if (!l->canonical) {
        if (c == 0x03) {
            interrupt_pgid = l->fg_pgid;
        } else {
            ready_push(l, &c, 1);
            if (l->echo_on && c >= 0x20 && c < 0x7F) {
                out[0] = c;
                out_len = 1;
            }
        }
        bool woke = l->head != l->tail;
        spin_unlock_irqrestore(&l->lock, flags);
        if (woke)
            wake_up_all(&l->readers);
        if (out_len && l->echo)
            l->echo(l->ctx, out, out_len);
        if (interrupt_pgid > 0)
            process_kill(-interrupt_pgid, SIGINT);
        return;
    }
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
        interrupt_pgid = l->fg_pgid;
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
    bool ready = l->head != l->tail || l->eof;
    spin_unlock_irqrestore(&l->lock, flags);

    if (ready)
        wake_up_all(&l->readers);
    if (out_len && l->echo && l->echo_on)
        l->echo(l->ctx, out, out_len);
    if (interrupt_pgid > 0)
        process_kill(-interrupt_pgid, SIGINT);
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

void ldisc_set_pgrp(struct line_discipline *l, int32_t pgid)
{
    l->fg_pgid = pgid;
}

int32_t ldisc_get_pgrp(struct line_discipline *l)
{
    return l->fg_pgid;
}

void ldisc_hangup(struct line_discipline *l)
{
    uint64_t flags = spin_lock_irqsave(&l->lock);
    l->hangup = true;
    spin_unlock_irqrestore(&l->lock, flags);
    wake_up_all(&l->readers);
}

int64_t ldisc_read(struct line_discipline *l, void *buf, uint64_t len)
{
    char *out = buf;
    uint64_t flags = spin_lock_irqsave(&l->lock);
    for (;;) {
        uint64_t n = 0;
        while (n < len && l->tail != l->head) {
            out[n++] = l->ready[l->tail];
            l->tail = (l->tail + 1) % LDISC_READY_MAX;
        }
        if (n) {
            spin_unlock_irqrestore(&l->lock, flags);
            return (int64_t)n;
        }
        if (l->eof) {
            l->eof = false;
            spin_unlock_irqrestore(&l->lock, flags);
            return 0;
        }
        if (l->hangup) {
            spin_unlock_irqrestore(&l->lock, flags);
            return 0;
        }
        wait_sleep_intr(&l->readers, &l->lock, 0);
        if (signal_pending_current()) {
            spin_unlock_irqrestore(&l->lock, flags);
            return -EINTR;
        }
    }
}

short ldisc_poll(struct line_discipline *l, struct poll_table *pt)
{
    poll_wait(pt, &l->readers);
    uint64_t flags = spin_lock_irqsave(&l->lock);
    short revents = 0;
    if (l->tail != l->head || l->eof)
        revents |= POLLIN;
    if (l->hangup)
        revents |= POLLHUP;
    spin_unlock_irqrestore(&l->lock, flags);
    return revents;
}
