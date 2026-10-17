#pragma once

#include <sys/types.h>

struct sysinfo_orion {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t shared_pages;
    uint64_t cow_faults;
    uint64_t uptime_ms;
    uint64_t heap_used;
    uint32_t cpus;
    uint32_t procs;
};

#define PROCLIST_NAME 24
#define PROCLIST_MAX  256

struct procinfo_orion {
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t state;
    uint64_t pages;
    char     name[PROCLIST_NAME];
};

int sysinfo_orion(struct sysinfo_orion *out);
int proclist_orion(struct procinfo_orion *out, int max);
