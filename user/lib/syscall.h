#pragma once

#include <stdint.h>
#include <stddef.h>

#define SYS_WRITE       1
#define SYS_SCHED_YIELD 24
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_EXIT        60

static inline long syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long n, long a) { return syscall3(n, a, 0, 0); }
static inline long syscall0(long n)         { return syscall3(n, 0, 0, 0); }