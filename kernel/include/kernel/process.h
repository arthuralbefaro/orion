#pragma once

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
#define USER_HEAP_MAX    (64 * 1024 * 1024)
#define PROCESS_CWD_MAX  256

struct thread;
struct file;
struct vma;
struct interrupt_frame;

enum process_state { PROC_RUNNING, PROC_ZOMBIE };

struct process {
    uint32_t pid;
    char name[PROCESS_NAME_MAX];
    enum process_state state;
    uint64_t pml4_phys;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t brk_start;
    uint64_t brk;
    uint64_t pages;
    struct vma *vmas;
    char cwd[PROCESS_CWD_MAX];
    int exit_code;
    uint32_t pgid;
    uint64_t sig_pending;
    uint64_t alarm_at;
    struct user_sigaction *sig_handlers;
    bool autofree;
    struct thread *threads;
    uint32_t live_threads;
    uint32_t thread_refs;
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
    struct process *next;
};

struct process *process_create_elf(const char *name, const void *image, size_t size);
struct process *process_spawn_path(const char *path, const char *const *argv);
struct process *process_current(void);
struct process *process_find(uint32_t pid);

int  process_fork(struct interrupt_frame *frame);
int  process_fork_stack(struct interrupt_frame *frame, uint64_t newsp);
int  process_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *frame);
int  process_waitpid(int pid, int *status, int options);
NORETURN void process_exit(int code);
NORETURN void process_exit_thread(int code);
NORETURN void process_thread_leave(void);
long process_clone(struct interrupt_frame *frame, uint64_t flags, uint64_t newsp, uint64_t ptid,
                   uint64_t ctid, uint64_t tls);
struct file *process_fd_pin(int fd);
void process_unpin_all(void);
void process_mm_tlb_sync(struct process *p);
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

bool process_resolve_path(struct process *p, const char *path, char *out, size_t out_size);

void process_dump(void);
int  process_list(struct user_procinfo *out, int max);
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
