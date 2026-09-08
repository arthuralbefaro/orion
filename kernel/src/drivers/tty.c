#include <drivers/tty.h>
#include <drivers/ldisc.h>
#include <lib/printf.h>
#include <kernel/log.h>

static struct line_discipline ldisc;

static void tty_echo(void *ctx, const char *data, uint64_t len)
{
    (void)ctx;
    uint64_t flags;
    bool locked = kprint_lock(&flags);
    for (uint64_t i = 0; i < len; i++)
        kputc(data[i]);
    kprint_unlock(locked, flags);
}

void tty_init(void)
{
    ldisc_init(&ldisc, tty_echo, NULL);
}

void tty_input(char c)
{
    ldisc_input(&ldisc, c);
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
    return ldisc_read(&ldisc, buf, len);
}

short tty_poll(struct poll_table *pt)
{
    return ldisc_poll(&ldisc, pt);
}

long tty_termios(uint64_t request, uint64_t arg)
{
    return ldisc_termios_ioctl(&ldisc, request, arg);
}

void tty_set_pgrp(int32_t pgid)
{
    ldisc_set_pgrp(&ldisc, pgid);
}

int32_t tty_get_pgrp(void)
{
    return ldisc_get_pgrp(&ldisc);
}
