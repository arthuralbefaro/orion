#pragma once

#include <kernel/compiler.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <kernel/syscall.h>

#define PROCESS_NAME_MAX 32
#define PROCESS_MAX_FDS  32
#define USER_STACK_TOP   0x00007ffffffff000ull
#define USER_STACK_SIZE  (64 * 1024)
#define USER_HEAP_MAX    (64 * 1024 * 1024)
#define PROCESS_CWD_MAX  256

struct thread;
struct file;
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
    uint64_t mmap_next;
    char cwd[PROCESS_CWD_MAX];
    int exit_code;
    uint32_t pgid;
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t alarm_at;
    struct user_sigaction sig_handlers[NSIG];
    bool autofree;
    struct thread *thread;
    struct process *parent;
    struct file *fds[PROCESS_MAX_FDS];
    struct process *next;
};

struct process *process_create_elf(const char *name, const void *image, size_t size);
struct process *process_spawn_path(const char *path, const char *const *argv);
struct process *process_current(void);
struct process *process_find(uint32_t pid);

int  process_fork(struct interrupt_frame *frame);
int  process_execve(const char *path, char *const *argv, char *const *envp, struct interrupt_frame *frame);
int  process_waitpid(int pid, int *status, int options);
NORETURN void process_exit(int code);
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
