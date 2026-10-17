#include <kernel/timesys.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <drivers/rtc.h>
#include <mm/uaccess.h>

#define NSEC_PER_SEC   1000000000ull
#define USEC_PER_SEC   1000000ull
#define MAX_SLEEP_SEC  1000000000ull
#define USER_CLK_TCK   100

static uint64_t realtime_us(void)
{
    return rtc_boot_epoch() * USEC_PER_SEC + time_now_us();
}

static void us_to_timespec(uint64_t us, struct user_timespec *ts)
{
    ts->tv_sec = (int64_t)(us / USEC_PER_SEC);
    ts->tv_nsec = (int64_t)(us % USEC_PER_SEC) * 1000;
}

static void ms_to_timespec(uint64_t ms, struct user_timespec *ts)
{
    ts->tv_sec = (int64_t)(ms / 1000);
    ts->tv_nsec = (int64_t)(ms % 1000) * 1000000;
}

static void us_to_timeval(uint64_t us, struct user_timeval *tv)
{
    tv->tv_sec = (int64_t)(us / USEC_PER_SEC);
    tv->tv_usec = (int64_t)(us % USEC_PER_SEC);
}

static void ms_to_timeval(uint64_t ms, struct user_timeval *tv)
{
    tv->tv_sec = (int64_t)(ms / 1000);
    tv->tv_usec = (int64_t)(ms % 1000) * 1000;
}

static bool valid_timespec(const struct user_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && (uint64_t)ts->tv_nsec < NSEC_PER_SEC;
}

static uint64_t timespec_to_us(const struct user_timespec *ts)
{
    if ((uint64_t)ts->tv_sec >= MAX_SLEEP_SEC)
        return MAX_SLEEP_SEC * USEC_PER_SEC;
    return (uint64_t)ts->tv_sec * USEC_PER_SEC + ((uint64_t)ts->tv_nsec + 999) / 1000;
}

static uint64_t abs_timespec_to_us(const struct user_timespec *ts)
{
    if ((uint64_t)ts->tv_sec >= 1000000000000ull)
        return 1000000000000ull * USEC_PER_SEC;
    return (uint64_t)ts->tv_sec * USEC_PER_SEC + ((uint64_t)ts->tv_nsec + 999) / 1000;
}

static bool cpu_clock(int clk, struct user_timespec *ts)
{
    struct process *p = process_current();
    if (clk == CLOCK_THREAD_CPUTIME_ID) {
        uint64_t u, s;
        thread_cpu_times(thread_current(), &u, &s);
        us_to_timespec(u + s, ts);
        return true;
    }
    if (clk == CLOCK_PROCESS_CPUTIME_ID && p) {
        struct cpu_times self, children;
        process_cpu_times(p, &self, &children);
        us_to_timespec(self.utime + self.stime, ts);
        return true;
    }
    return false;
}

long sys_clock_gettime(int clk, struct user_timespec *uts)
{
    struct user_timespec ts;
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_TAI:
        us_to_timespec(realtime_us(), &ts);
        break;
    case CLOCK_REALTIME_COARSE:
        ms_to_timespec(realtime_us() / 1000, &ts);
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        us_to_timespec(time_now_us(), &ts);
        break;
    case CLOCK_MONOTONIC_COARSE:
        ms_to_timespec(time_now_us() / 1000, &ts);
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        if (!cpu_clock(clk, &ts))
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }
    return copy_to_user(uts, &ts, sizeof(ts));
}

long sys_clock_getres(int clk, struct user_timespec *ures)
{
    struct user_timespec ts = { 0, 0 };
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_TAI:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        ts.tv_nsec = 1000;
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        ts.tv_nsec = 1000;
        break;
    case CLOCK_REALTIME_COARSE:
    case CLOCK_MONOTONIC_COARSE:
        ts.tv_nsec = 1000000;
        break;
    default:
        return -EINVAL;
    }
    if (!ures)
        return 0;
    return copy_to_user(ures, &ts, sizeof(ts));
}

long sys_gettimeofday(struct user_timeval *utv, void *utz)
{
    if (utv) {
        uint64_t us = realtime_us();
        struct user_timeval tv = { (int64_t)(us / USEC_PER_SEC), (int64_t)(us % USEC_PER_SEC) };
        if (copy_to_user(utv, &tv, sizeof(tv)) < 0)
            return -EFAULT;
    }
    if (utz) {
        int32_t tz[2] = { 0, 0 };
        if (copy_to_user(utz, tz, sizeof(tz)) < 0)
            return -EFAULT;
    }
    return 0;
}

long sys_time(int64_t *ut)
{
    int64_t now = (int64_t)(realtime_us() / USEC_PER_SEC);
    if (ut && copy_to_user(ut, &now, sizeof(now)) < 0)
        return -EFAULT;
    return now;
}

static long sleep_until_mono(uint64_t deadline_us)
{
    spinlock_t lock = SPINLOCK_INIT;
    struct waitqueue wq = WAITQUEUE_INIT;
    for (;;) {
        uint64_t now = time_now_us();
        if (now >= deadline_us)
            return 0;
        if (signal_pending_current())
            return -EINTR;
        uint64_t ms = (deadline_us - now + 999) / 1000;
        if (ms > 86400000)
            ms = 86400000;
        uint64_t flags = spin_lock_irqsave(&lock);
        wait_sleep_intr(&wq, &lock, ms);
        spin_unlock_irqrestore(&lock, flags);
    }
}

static long write_rem(struct user_timespec *urem, uint64_t deadline_us)
{
    if (!urem)
        return 0;
    uint64_t now = time_now_us();
    struct user_timespec rem = { 0, 0 };
    if (deadline_us > now)
        us_to_timespec(deadline_us - now, &rem);
    return copy_to_user(urem, &rem, sizeof(rem));
}

static long sleep_restartable(uint64_t deadline, struct user_timespec *urem)
{
    long r = sleep_until_mono(deadline);
    if (r != -EINTR)
        return r;
    if (write_rem(urem, deadline) < 0)
        return -EFAULT;
    struct thread *t = thread_current();
    t->restart_kind = RESTART_SLEEP;
    t->restart_deadline_us = deadline;
    t->restart_arg[0] = (uint64_t)urem;
    return -ERESTART_RESTARTBLOCK;
}

long timesys_restart_sleep(void)
{
    struct thread *t = thread_current();
    return sleep_restartable(t->restart_deadline_us, (struct user_timespec *)t->restart_arg[0]);
}

long sys_nanosleep(const struct user_timespec *ureq, struct user_timespec *urem)
{
    struct user_timespec req;
    if (copy_from_user(&req, ureq, sizeof(req)) < 0)
        return -EFAULT;
    if (!valid_timespec(&req))
        return -EINVAL;
    return sleep_restartable(time_now_us() + timespec_to_us(&req), urem);
}

long sys_clock_nanosleep(int clk, int flags, const struct user_timespec *ureq, struct user_timespec *urem)
{
    bool realtime;
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_TAI:
        realtime = true;
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_BOOTTIME:
        realtime = false;
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
    struct user_timespec req;
    if (copy_from_user(&req, ureq, sizeof(req)) < 0)
        return -EFAULT;
    if (!valid_timespec(&req))
        return -EINVAL;
    uint64_t now = time_now_us();
    uint64_t deadline;
    if (flags & TIMER_ABSTIME) {
        uint64_t target = abs_timespec_to_us(&req);
        if (realtime) {
            uint64_t base = rtc_boot_epoch() * USEC_PER_SEC;
            deadline = target > base ? target - base : 0;
        } else {
            deadline = target;
        }
    } else {
        deadline = now + timespec_to_us(&req);
    }
    if (flags & TIMER_ABSTIME) {
        long r = sleep_until_mono(deadline);
        return r == -EINTR ? -ERESTARTNOHAND : r;
    }
    return sleep_restartable(deadline, urem);
}

static void fill_rusage(const struct cpu_times *t, struct user_rusage *ru)
{
    for (size_t i = 0; i < sizeof(*ru); i++)
        ((uint8_t *)ru)[i] = 0;
    us_to_timeval(t->utime, &ru->ru_utime);
    us_to_timeval(t->stime, &ru->ru_stime);
}

long rusage_to_user(const struct cpu_times *t, struct user_rusage *uru)
{
    struct user_rusage ru;
    fill_rusage(t, &ru);
    return copy_to_user(uru, &ru, sizeof(ru));
}

long sys_getrusage(int who, struct user_rusage *uru)
{
    struct process *p = process_current();
    struct cpu_times self, children;
    switch (who) {
    case RUSAGE_SELF:
        process_cpu_times(p, &self, &children);
        return rusage_to_user(&self, uru);
    case RUSAGE_CHILDREN:
        process_cpu_times(p, &self, &children);
        return rusage_to_user(&children, uru);
    case RUSAGE_THREAD: {
        struct cpu_times mine;
        thread_cpu_times(thread_current(), &mine.utime, &mine.stime);
        return rusage_to_user(&mine, uru);
    }
    default:
        return -EINVAL;
    }
}

long sys_times(struct user_tms *ubuf)
{
    struct process *p = process_current();
    if (ubuf) {
        struct cpu_times self, children;
        process_cpu_times(p, &self, &children);
        struct user_tms t;
        uint64_t div = USEC_PER_SEC / USER_CLK_TCK;
        t.tms_utime = (int64_t)(self.utime / div);
        t.tms_stime = (int64_t)(self.stime / div);
        t.tms_cutime = (int64_t)(children.utime / div);
        t.tms_cstime = (int64_t)(children.stime / div);
        if (copy_to_user(ubuf, &t, sizeof(t)) < 0)
            return -EFAULT;
    }
    return (long)(time_now_us() / (USEC_PER_SEC / USER_CLK_TCK));
}

static uint64_t timeval_to_ms(const struct user_timeval *tv)
{
    if ((uint64_t)tv->tv_sec >= MAX_SLEEP_SEC)
        return MAX_SLEEP_SEC * 1000;
    uint64_t ms = (uint64_t)tv->tv_sec * 1000 + ((uint64_t)tv->tv_usec + 999) / 1000;
    return ms;
}

static long itimer_to_user(uint64_t value_ms, uint64_t interval_ms, struct user_itimerval *uit)
{
    struct user_itimerval it;
    ms_to_timeval(interval_ms, &it.it_interval);
    ms_to_timeval(value_ms, &it.it_value);
    return copy_to_user(uit, &it, sizeof(it));
}

long sys_getitimer(int which, struct user_itimerval *uit)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    uint64_t value, interval;
    process_itimer(false, 0, 0, &value, &interval);
    return itimer_to_user(value, interval, uit);
}

long sys_setitimer(int which, const struct user_itimerval *unew, struct user_itimerval *uold)
{
    if (which != ITIMER_REAL)
        return -EINVAL;
    struct user_itimerval it = { { 0, 0 }, { 0, 0 } };
    if (unew && copy_from_user(&it, unew, sizeof(it)) < 0)
        return -EFAULT;
    if (it.it_value.tv_sec < 0 || it.it_value.tv_usec < 0 || it.it_value.tv_usec >= 1000000 ||
        it.it_interval.tv_sec < 0 || it.it_interval.tv_usec < 0 || it.it_interval.tv_usec >= 1000000)
        return -EINVAL;
    uint64_t old_value, old_interval;
    process_itimer(true, timeval_to_ms(&it.it_value), timeval_to_ms(&it.it_interval), &old_value, &old_interval);
    if (uold)
        return itimer_to_user(old_value, old_interval, uold);
    return 0;
}
