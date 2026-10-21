#pragma once

#include <kernel/compiler.h>

#define MAX_CPUS 64
#define IRQ_RING 16

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
    uint64_t switch_us;
    uint64_t user_ticks;
    uint64_t sys_ticks;
    uint64_t idle_ticks;
    uint32_t irq_depth;
    uint32_t irq_depth_max;
    uint8_t irq_vector;
    bool in_nmi;
    const char *nmi_bug_what;
    uint64_t nmi_bug_caller;
    uint64_t preempt_deferred;
    struct thread *rq_head;
    struct thread *rq_tail;
    uint32_t nr_ready;
    uint64_t rq_load;
    uint64_t min_vruntime;
    uint64_t migrations;
    uint64_t steals;
    uint64_t balance_moves;
    uint64_t resched_ipis;
    uint64_t preempt_unlock;
    uint8_t irq_ring[IRQ_RING];
    uint8_t irq_ring_pos;
    uint8_t irq_top_vector;
    uint8_t irq_last_top_vector;
    uint32_t irq_top_count;
    uint32_t irq_last_top_count;
    uint64_t irq_rate_epoch;
    uint32_t irq_rate[256];
};

void percpu_init_bsp(uint32_t lapic_id);
void percpu_init_ap(uint32_t index, uint32_t lapic_id);
bool percpu_ready(void);
struct percpu *percpu_get(void);
struct percpu *percpu_by_index(uint32_t index);
uint32_t percpu_cpu_id(void);
uint32_t percpu_count(void);
void percpu_set_kernel_rsp(uint64_t rsp);
