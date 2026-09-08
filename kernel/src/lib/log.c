/*
 * log.c - klog() e panic().
 */
#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <drivers/console.h>
#include <kernel/smp.h>

static const char *level_tag[] = {
    [LOG_DEBUG] = "dbg ",
    [LOG_INFO]  = "info",
    [LOG_WARN]  = "WARN",
    [LOG_ERROR] = "ERR ",
};

static uint32_t level_color[] = {
    [LOG_DEBUG] = COLOR_DARK_GRAY,
    [LOG_INFO]  = COLOR_GREEN,
    [LOG_WARN]  = COLOR_YELLOW,
    [LOG_ERROR] = COLOR_RED,
};

void klog(enum log_level level, const char *subsystem, const char *fmt, ...)
{
    uint64_t flags;
    bool locked = kprint_lock(&flags);
    uint32_t fg, bg;
    console_get_colors(&fg, &bg);

    console_set_colors(level_color[level], bg);
    kprintf("[%s] ", level_tag[level]);
    console_set_colors(COLOR_CYAN, bg);
    kprintf("%-8s ", subsystem);
    console_set_colors(fg, bg);

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kputc('\n');
    kprint_unlock(locked, flags);
}

NORETURN void panic(const char *fmt, ...)
{
    cli();
    smp_halt_others();

    console_set_graphics(false);
    console_set_colors(COLOR_WHITE, COLOR_RED);
    kprintf("\n*** KERNEL PANIC ***\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\nSistema travado. Reinicie a maquina.\n");
    halt_forever();
}
