#include <drivers/rtc.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint64_t boot_epoch;

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, 0x80 | reg);
    return inb(CMOS_DATA);
}

static bool updating(void)
{
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

bool rtc_read(struct rtc_time *out)
{
    for (int tries = 0; tries < 1000000 && updating(); tries++)
        pause();
    uint8_t sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    uint8_t century = cmos_read(0x32);
    uint8_t regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {
        sec = bcd(sec); min = bcd(min);
        hour = (uint8_t)(bcd(hour & 0x7F) | (hour & 0x80));
        day = bcd(day); mon = bcd(mon); year = bcd(year);
        century = bcd(century);
    }
    if (!(regb & 0x02) && (hour & 0x80))
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);

    out->second = sec;
    out->minute = min;
    out->hour = hour;
    out->day = day;
    out->month = mon;
    out->year = (uint16_t)((century >= 19 && century <= 21 ? century : 20) * 100 + year);
    return out->month >= 1 && out->month <= 12 && out->day >= 1 && out->day <= 31;
}

uint64_t rtc_to_epoch(const struct rtc_time *t)
{
    int64_t y = t->year, m = t->month, d = t->day;
    if (m <= 2) { y--; m += 12; }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m - 3) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return (uint64_t)(days * 86400 + t->hour * 3600 + t->minute * 60 + t->second);
}

void rtc_init(void)
{
    struct rtc_time t;
    if (!rtc_read(&t)) {
        log_warn("rtc", "leitura invalida");
        return;
    }
    boot_epoch = rtc_to_epoch(&t);
    log_info("rtc", "%04u-%02u-%02u %02u:%02u:%02u UTC (epoch %lu)", t.year, t.month, t.day,
             t.hour, t.minute, t.second, boot_epoch);
}

uint64_t rtc_boot_epoch(void)
{
    return boot_epoch;
}
