#pragma once

#include <kernel/compiler.h>

#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)
#define SCHED_TIME_SLICE  10
#define THREAD_STACK_CANARY 0x4F52494F4E535450ull

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE,
};

struct process;
struct wait_entry;

typedef void (*thread_entry_t)(void *arg);

struct thread {
    uint64_t rsp;
    uint32_t tid;
    enum thread_state state;
    char name[THREAD_NAME_MAX];

    void   *stack;
    size_t  stack_size;
    uint64_t kernel_stack_top;
    struct process *process;

    uint64_t slice_left;
    uint64_t total_ticks;
    uint64_t wake_at;
    uint32_t cpu;
    bool     is_idle;
    uint64_t fs_base;
    bool     on_sleep_list;
    bool     on_runq;
    struct wait_entry *waits;

    struct thread *next;
    struct thread *all_next;
};

void sched_init(void);
void sched_init_ap(void);
bool sched_enabled(void);

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg);
struct thread *thread_create_for_process(const char *name, thread_entry_t entry, void *arg,
                                         struct process *process);
struct thread *thread_current(void);

void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
void thread_block(void);
void thread_unblock(struct thread *t);
NORETURN void thread_exit(void);

void schedule(void);
void sched_post_switch(void);

void sched_runq_lock(void);
void sched_runq_unlock(void);
bool sched_runq_held(void);
void sched_block_switch(uint64_t wake_at);
bool sched_make_ready(struct thread *t);
uint64_t sched_deadline(uint64_t ms);
void sched_tick(uint64_t now_ticks);
void sched_check_preempt(void);

void sched_dump(void);
