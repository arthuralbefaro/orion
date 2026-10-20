#pragma once

#include <kernel/compiler.h>
#include <kernel/signal.h>

#define THREAD_NAME_MAX   24
#define THREAD_STACK_SIZE (16 * 1024)
#define SCHED_TIME_SLICE  10
#define THREAD_STACK_CANARY 0x4F52494F4E535450ull
#define THREAD_PINS 40
#define NICE_MIN (-20)
#define NICE_MAX 19

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE,
};

struct process;
struct file;
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
    void    *fpu;
    uint64_t clear_child_tid;
    uint32_t utid;
    bool     kill_me;
    uint64_t sig_blocked;
    uint64_t sig_pending;
    uint64_t saved_mask;
    uint64_t sig_waiting;
    bool     restore_mask;
    uint64_t utime;
    uint64_t stime;
    uint64_t run_us;
    uint64_t prev_utime;
    uint64_t prev_stime;
    int      restart_kind;
    uint64_t restart_deadline_us;
    uint64_t restart_arg[2];
    struct sig_src sig_src[NSIG - 1];
    struct thread *proc_next;
    struct file *pins[THREAD_PINS];
    int      npins;
    struct wait_entry *waits;

    struct thread *next;
    struct thread *all_next;
    uint32_t spin_count;
    int      nice;
    uint64_t vruntime;
    bool     vr_fresh;
    uint64_t allowed;
};

void sched_init(void);
void sched_init_ap(void);
bool sched_enabled(void);

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg);
struct thread *thread_create_for_process_paused(const char *name, thread_entry_t entry, void *arg,
                                                struct process *process);
struct thread *thread_create_for_process(const char *name, thread_entry_t entry, void *arg,
                                         struct process *process);
struct thread *thread_current(void);
void thread_cpu_times(struct thread *t, uint64_t *user_us, uint64_t *sys_us);

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
void sched_tick(uint64_t now_ticks, bool user);
void sched_check_preempt(void);

struct sched_cpu_stat {
    uint32_t cpu;
    uint32_t nr_ready;
    uint64_t load;
    uint64_t min_vruntime;
    uint64_t context_switches;
    uint64_t migrations;
    uint64_t steals;
    uint64_t balance_moves;
    uint64_t resched_ipis;
    bool busy;
};

NORETURN void sched_idle(void);
uint64_t sched_online_mask(void);
long sched_set_affinity(struct thread *t, uint64_t mask);
uint64_t sched_get_affinity(struct thread *t);
void sched_set_nice(struct thread *t, int nice);
bool sched_cpu_stat(uint32_t index, struct sched_cpu_stat *out);
uint32_t sched_weight(int nice);

void sched_dump(void);
