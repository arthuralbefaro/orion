#include "ulib.h"
#include "syscall.h"

struct timespec { long tv_sec; long tv_nsec; };

long write(int fd, const void *buf, size_t len)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

void exit(int code)
{
    syscall1(SYS_EXIT, code);
    for (;;)
        ;
}

int getpid(void)
{
    return (int)syscall0(SYS_GETPID);
}

void yield(void)
{
    syscall0(SYS_SCHED_YIELD);
}

void sleep_ms(unsigned long ms)
{
    struct timespec ts = { (long)(ms / 1000), (long)(ms % 1000) * 1000000 };
    syscall1(SYS_NANOSLEEP, (long)&ts);
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

void puts(const char *s)
{
    write(1, s, strlen(s));
}

void print_num(unsigned long n)
{
    char buf[24];
    int i = sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    puts(&buf[i]);
}