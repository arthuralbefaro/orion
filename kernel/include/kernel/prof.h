#pragma once

#include <kernel/compiler.h>

struct interrupt_frame;

#define PROF_START 1
#define PROF_STOP 2
#define PROF_READ 3
#define PROF_STATS 4
#define PROF_SPIN_IRQOFF 5

struct prof_sample {
    uint64_t rip;
    uint32_t pid;
    uint16_t cpu;
    uint8_t user;
    uint8_t irq_off;
};

struct prof_stats {
    uint64_t samples;
    uint64_t dropped;
    uint64_t elapsed_ms;
    uint32_t hz;
    uint32_t cpus;
};

void prof_tick(struct interrupt_frame *f);
bool prof_nmi(struct interrupt_frame *f);
long sys_prof(uint64_t op, uint64_t arg, uint64_t ubuf, uint64_t max);
