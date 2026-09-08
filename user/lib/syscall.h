#pragma once

#include <stdint.h>
#include <stddef.h>

#define SYS_READ          0
#define SYS_POLL          7
#define SYS_FCNTL         72
#define SYS_RT_SIGACTION  13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN  15
#define SYS_ALARM         37
#define SYS_KILL          62
#define SYS_SETPGID       109
#define SYS_GETPGID       121
#define SYS_SOCKET        41
#define SYS_CONNECT       42
#define SYS_SENDTO        44
#define SYS_RECVFROM      45
#define SYS_BIND          49
#define SYS_RENAME        82
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_FSYNC         74
#define SYS_FTRUNCATE     77
#define SYS_SYNC          162
#define SYS_WRITE         1
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_STAT          4
#define SYS_FSTAT         5
#define SYS_LSEEK         8
#define SYS_MMAP          9
#define SYS_MUNMAP        11
#define SYS_BRK           12
#define SYS_IOCTL         16
#define SYS_PIPE          22
#define SYS_SCHED_YIELD   24
#define SYS_DUP           32
#define SYS_DUP2          33
#define SYS_NANOSLEEP     35
#define SYS_GETPID        39
#define SYS_FORK          57
#define SYS_EXECVE        59
#define SYS_EXIT          60
#define SYS_WAIT4         61
#define SYS_GETCWD        79
#define SYS_CHDIR         80
#define SYS_GETPPID       110
#define SYS_ARCH_PRCTL    158
#define SYS_CLOCK_GETTIME 228
#define SYS_READDIR       1000
#define SYS_OPENPTY       1001

static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall3(long n, long a, long b, long c) { return syscall6(n, a, b, c, 0, 0, 0); }
static inline long syscall2(long n, long a, long b)         { return syscall6(n, a, b, 0, 0, 0, 0); }
static inline long syscall1(long n, long a)                 { return syscall6(n, a, 0, 0, 0, 0, 0); }
static inline long syscall0(long n)                         { return syscall6(n, 0, 0, 0, 0, 0, 0); }

long __syscall_ret(long r);
