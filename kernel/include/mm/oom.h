#pragma once

#include <kernel/compiler.h>

struct oom_stats {
    uint64_t kills;
    uint64_t self_kills;
    uint64_t reclaims;
};

#define OOM_FAULT_TRIES 50

bool oom_recover(void);
bool oom_fatal_pending(void);
void oom_stats(struct oom_stats *out);
