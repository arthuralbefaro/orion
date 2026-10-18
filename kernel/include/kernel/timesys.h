#pragma once

#include <kernel/compiler.h>

#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_TAI                11

#define TIMER_ABSTIME 1

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

#define ITIMER_REAL 0

#define RESTART_NONE  0
#define RESTART_SLEEP 1
#define RESTART_POLL  2

struct user_timespec;

struct user_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct user_itimerval {
    struct user_timeval it_interval;
    struct user_timeval it_value;
};

struct user_rusage {
    struct user_timeval ru_utime;
    struct user_timeval ru_stime;
    int64_t rest[14];
};

struct user_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

struct cpu_times;

long sys_clock_gettime(int clk, struct user_timespec *uts);
long sys_clock_getres(int clk, struct user_timespec *ures);
long sys_gettimeofday(struct user_timeval *utv, void *utz);
long sys_time(int64_t *ut);
long sys_nanosleep(const struct user_timespec *ureq, struct user_timespec *urem);
long sys_clock_nanosleep(int clk, int flags, const struct user_timespec *ureq, struct user_timespec *urem);
long sys_getrusage(int who, struct user_rusage *uru);
long sys_times(struct user_tms *ubuf);
long sys_getitimer(int which, struct user_itimerval *uit);
long sys_setitimer(int which, const struct user_itimerval *unew, struct user_itimerval *uold);
long rusage_to_user(const struct cpu_times *t, struct user_rusage *uru);
long timesys_restart_sleep(void);
void timesys_realtime(int64_t *sec, uint32_t *nsec);
