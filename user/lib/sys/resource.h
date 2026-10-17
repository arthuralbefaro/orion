#pragma once

#include <sys/types.h>

#define RLIMIT_STACK  3
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIM_INFINITY (~0ul)

typedef unsigned long rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

int getrlimit(int resource, struct rlimit *r);
int setrlimit(int resource, const struct rlimit *r);
