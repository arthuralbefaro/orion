/*
 * time.c - tick do sistema e TSC.
 */
#include <kernel/time.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <drivers/pit.h>
#include <drivers/pic.h>
#include <arch/x86_64/percpu.h>

static volatile uint64_t ticks;
static const char *source = "nenhuma";
static uint64_t tsc_hz;
static uint64_t tsc_boot;

static void tick_handler(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    if (percpu_cpu_id() == 0)
        ticks++;
    sched_tick(ticks);
}

/* Mede quantos ciclos de TSC cabem em 10 ms (usando o canal 2 do PIT em
 * polling, sem depender de interrupções). */
static uint64_t calibrate_tsc(void)
{
    uint64_t t0 = rdtsc();
    pit_channel2_wait_ms(10);
    uint64_t t1 = rdtsc();
    return (t1 - t0) * 100;
}

void time_init(void)
{
    tsc_boot = rdtsc();
    tsc_hz = calibrate_tsc();

    if (lapic_is_enabled() && lapic_timer_calibrate()) {
        interrupts_register(VECTOR_LAPIC_TIMER, tick_handler, NULL);
        lapic_timer_start_periodic(VECTOR_LAPIC_TIMER, TIME_TICK_HZ);
        source = "LAPIC timer";
    } else {
        interrupts_register(VECTOR_IRQ_BASE + IRQ_TIMER, tick_handler, NULL);
        pit_init(TIME_TICK_HZ);
        source = "PIT";
    }

    log_info("time", "tick de %u Hz via %s; TSC a %lu.%03lu MHz",
             TIME_TICK_HZ, source, tsc_hz / 1000000, (tsc_hz / 1000) % 1000);
}

void time_init_ap(void)
{
    if (lapic_timer_frequency())
        lapic_timer_start_periodic(VECTOR_LAPIC_TIMER, TIME_TICK_HZ);
}

uint64_t time_ticks(void)
{
    return ticks;
}

uint64_t time_uptime_ms(void)
{
    return ticks * 1000 / TIME_TICK_HZ;
}

uint64_t time_now_us(void)
{
    if (!tsc_hz)
        return ticks * (1000000 / TIME_TICK_HZ);
    return (rdtsc() - tsc_boot) / (tsc_hz / 1000000);
}

void time_sleep_ms(uint64_t ms)
{
    ASSERT(interrupts_enabled());
    uint64_t target = ticks + DIV_ROUND_UP(ms * TIME_TICK_HZ, 1000);
    while (ticks < target)
        hlt();
}

void time_udelay(uint64_t us)
{
    uint64_t end = time_now_us() + us;
    while (time_now_us() < end)
        pause();
}

const char *time_source_name(void)
{
    return source;
}

uint64_t time_tsc_hz(void)
{
    return tsc_hz;
}
