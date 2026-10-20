#include <kernel/log.h>
#include <arch/x86_64/cpu.h>
#include <drivers/console.h>
#include <kernel/smp.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/time.h>

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

void kvlog(enum log_level level, const char *subsystem, const char *fmt, va_list ap)
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

    kvprintf(fmt, ap);

    kputc('\n');
    kprint_unlock(locked, flags);
}

void klog(enum log_level level, const char *subsystem, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvlog(level, subsystem, fmt, ap);
    va_end(ap);
}

void klog_limited(struct log_limit *lim, enum log_level level, const char *subsystem, const char *fmt, ...)
{
    uint64_t sec = time_ticks() / 1000 + 1;
    uint64_t w = __atomic_load_n(&lim->window, __ATOMIC_ACQUIRE);
    uint32_t dropped = 0;
    if (w != sec && __atomic_compare_exchange_n(&lim->window, &w, sec, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        dropped = __atomic_exchange_n(&lim->dropped, 0, __ATOMIC_ACQ_REL);
        __atomic_store_n(&lim->count, 0, __ATOMIC_RELEASE);
    }
    if (__atomic_add_fetch(&lim->count, 1, __ATOMIC_ACQ_REL) > LOG_LIMIT_PER_SECOND) {
        __atomic_add_fetch(&lim->dropped, 1, __ATOMIC_ACQ_REL);
        return;
    }
    if (dropped)
        klog(level, subsystem, "(%u mensagem(ns) parecida(s) suprimida(s): limite de %u por segundo)", dropped,
             LOG_LIMIT_PER_SECOND);
    va_list ap;
    va_start(ap, fmt);
    kvlog(level, subsystem, fmt, ap);
    va_end(ap);
}

NORETURN void panic(const char *fmt, ...)
{
    cli();
    smp_halt_others();
    kprint_panic_mode();

    console_set_graphics(false);
    console_set_colors(COLOR_WHITE, COLOR_RED);
    kprintf("\n*** KERNEL PANIC ***\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    interrupts_backtrace_here();

    kprintf("\nSistema travado. Reinicie a maquina.\n");
    halt_forever();
}
