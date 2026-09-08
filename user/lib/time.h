#pragma once

#include <sys/types.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

int    clock_gettime(int clk, struct timespec *ts);
int    nanosleep(const struct timespec *req, struct timespec *rem);
time_t time(time_t *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *gmtime(const time_t *t);
size_t strftime(char *buf, size_t max, const char *fmt, const struct tm *tm);
