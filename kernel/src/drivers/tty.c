#include <drivers/tty.h>
#include <drivers/ldisc.h>
#include <lib/printf.h>

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
