#pragma once

#include <kernel/compiler.h>

struct rtc_time {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
};

void rtc_init(void);
bool rtc_read(struct rtc_time *out);
uint64_t rtc_boot_epoch(void);
uint64_t rtc_to_epoch(const struct rtc_time *t);
