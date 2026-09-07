#pragma once

#include <kernel/compiler.h>

#define TIME_TICK_HZ 1000

void     time_init(void);
void     time_init_ap(void);
uint64_t time_ticks(void);
uint64_t time_uptime_ms(void);
uint64_t time_now_us(void);
void     time_sleep_ms(uint64_t ms);
void     time_udelay(uint64_t us);

const char *time_source_name(void);
uint64_t time_tsc_hz(void);