#pragma once

#include <kernel/cred.h>
#include <kernel/compiler.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>
#include <kernel/spinlock.h>
#include <kernel/mutex.h>

#define PROCESS_NAME_MAX 32
#define PROCESS_FD_INITIAL 32
#define PROCESS_FD_MAX     256
#define PROCESS_NPROC_MAX  512

#define RLIMIT_STACK  3
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_COUNT  16
#define RLIM_INFINITY (~0ull)

struct user_rlimit {
    uint64_t cur;
    uint64_t max;
};
#define USER_STACK_TOP   0x00007ffffffff000ull
#define USER_STACK_SIZE  (64 * 1024)
#define USER_STACK_MAX   (8 * 1024 * 1024)
#define USER_STACK_RANDOM_PAGES (1ull << 22)
#define USER_MMAP_RANDOM_PAGES  (1ull << 28)
#define USER_HEAP_MAX    (64 * 1024 * 1024)
#define PROCESS_CWD_MAX  256
#define PROCESS_EXE_MAX  128

struct thread;
struct file;
struct vma;
struct interrupt_frame;

enum process_state { PROC_RUNNING, PROC_ZOMBIE };

#define PROCESS_FREE_BATCH 32

struct cpu_times {
    uint64_t utime;
    uint64_t stime;
};

struct vnode;

struct process {
    uint32_t pid;
    char name[PROCESS_NAME_MAX];
    enum process_state state;
    uint64_t pml4_phys;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t mmap_base;
    uint64_t brk_start;
    uint64_t brk;
    uint64_t pages;
    struct vma *vmas;
    char cwd[PROCESS_CWD_MAX];
    struct vnode *cwd_vn;
    int exit_code;
    uint32_t pgid;
    uint64_t sig_pending;
    struct sig_src sig_src[NSIG - 1];
    uint64_t alarm_at;
    uint64_t alarm_interval;
    uint64_t dead_utime;
    uint64_t dead_stime;
    uint64_t child_utime;
    uint64_t child_stime;
    struct user_sigaction *sig_handlers;
    bool autofree;
    struct thread *threads;
    uint32_t live_threads;
    uint32_t thread_refs;
    uint32_t free_n;
    uint64_t free_batch[PROCESS_FREE_BATCH];
    bool execing;
    bool group_exit;
    int group_exit_status;
    spinlock_t tlock;
    struct mutex mm_lock;
    struct process *parent;
    struct file **fds;
    uint8_t *fd_flags;
    int fd_cap;
    struct user_rlimit rlimits[RLIMIT_COUNT];
    uint32_t umask;
    struct cred cred;
    spinlock_t cred_lock;
    char *cmdline;
    uint32_t cmdline_len;
    char exe[PROCESS_EXE_MAX];
    uint32_t pins;
    bool free_pending;
    struct process *next;
};

#define PROCESS_CMDLINE_MAX 4096

struct process *process_create_elf(const char *name, const void *image, size_t size);
struct process *process_spawn_path(const char *path, const char *const *argv);
struct process *process_current(void);
struct process *process_find(uint32_t pid);
struct process *process_get(uint32_t pid);
void process_put(struct process *p);
int process_pids(uint32_t *out, int max);
uint32_t process_ppid(struct process *p);
uint32_t process_cmdline(struct process *p, char *out, uint32_t size);

int  process_fork(struct interrupt_frame *frame);
int  process_fork_stack(struct interrupt_frame *frame, uint64_t newsp);
int  process_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *frame);
int  process_waitpid(int pid, int *status, int options, struct cpu_times *child);
void process_cpu_times(struct process *p, struct cpu_times *self, struct cpu_times *children);
long process_signal_thread(int tgid, int tid, int sig);
void process_itimer(bool set, uint64_t value_ms, uint64_t interval_ms, uint64_t *old_value, uint64_t *old_interval);
NORETURN void process_exit(int code);
NORETURN void process_exit_thread(int code);
NORETURN void process_thread_leave(void);
long process_clone(struct interrupt_frame *frame, uint64_t flags, uint64_t newsp, uint64_t ptid,
                   uint64_t ctid, uint64_t tls);
struct file *process_fd_pin(int fd);
void process_unpin_all(void);
void process_mm_tlb_sync(struct process *p);
void process_defer_free(struct process *p, uint64_t phys);
NORETURN void process_exit_signal(int sig);
int  process_kill(int pid, int sig);
int  process_setpgid(int pid, int pgid);
int  process_getpgid(int pid);
long process_alarm(uint64_t seconds);
void process_alarm_init(void);
void process_thread_reaped(struct process *p);

int  process_fd_install(struct process *p, struct file *f);
struct file *process_fd_get(struct process *p, int fd);
int  process_fd_close(struct process *p, int fd);
void process_fd_close_if(struct process *p, int fd, struct file *f);

void process_get_cwd(struct process *p, char *out, size_t size);
void process_set_cwd(struct process *p, const char *path, struct vnode *v);
struct vnode *process_cwd_vnode(struct process *p);
void process_update_cwd_path(struct process *p, struct vnode *v, const char *path);
bool process_resolve_path(struct process *p, const char *path, char *out, size_t out_size);

void process_dump(void);
int  process_count(void);
int  process_getppid(void);
long process_getrlimit(int resource, struct user_rlimit *out);
long process_setrlimit(int resource, const struct user_rlimit *in);
int  process_fd_limit(struct process *p);
bool process_fd_reserve(struct process *p, int fd);
int  process_fd_alloc(struct process *p, struct file *f, int min, bool cloexec);
int  process_fd_replace(struct process *p, int fd, struct file *f, bool cloexec);
int  process_fd_get_flags(struct process *p, int fd);
int  process_fd_set_flags(struct process *p, int fd, int flags);
