#pragma once

#include <kernel/compiler.h>

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256

struct user_timespec;

long futex_syscall(uint32_t *uaddr, int op, uint32_t val, uint64_t arg4, uint32_t *uaddr2, uint32_t val3);
long futex_wake(uint32_t *uaddr, int count, bool private_key);
uint64_t futex_waiters(void);
