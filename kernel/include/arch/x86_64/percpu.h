#pragma once

#include <kernel/compiler.h>

#define MAX_CPUS 64

struct thread;
struct tss;

struct percpu {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
    uint32_t cpu_id;
    uint32_t lapic_id;
    struct thread *current;
    struct percpu *self;
    struct thread *idle;
    struct thread *zombie;
    volatile bool need_resched;
    bool online;
    uint64_t ticks;
    uint64_t context_switches;
    volatile uint64_t heartbeat;
    uint64_t wd_seen;
    uint32_t wd_strikes;
};

void percpu_init_bsp(uint32_t lapic_id);
void percpu_init_ap(uint32_t index, uint32_t lapic_id);
bool percpu_ready(void);
struct percpu *percpu_get(void);
struct percpu *percpu_by_index(uint32_t index);
uint32_t percpu_cpu_id(void);
uint32_t percpu_count(void);
void percpu_set_kernel_rsp(uint64_t rsp);
