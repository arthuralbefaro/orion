#pragma once

#include <kernel/compiler.h>

#define SYS_WRITE       1
#define SYS_SCHED_YIELD 24
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_EXIT        60

#define ENOSYS 38
#define EFAULT 14
#define EBADF  9

#define USER_ADDR_MAX 0x0000800000000000ull

void syscall_init(void);
void syscall_init_cpu(void);