#pragma once

#include <kernel/compiler.h>

#define PROCESS_NAME_MAX 32
#define PROCESS_MAX_FDS  16
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
int  process_waitpid(int pid, int *status);
NORETURN void process_exit(int code);
void process_thread_reaped(struct process *p);

int  process_fd_install(struct process *p, struct file *f);
struct file *process_fd_get(struct process *p, int fd);
int  process_fd_close(struct process *p, int fd);

bool process_resolve_path(struct process *p, const char *path, char *out, size_t out_size);

void process_dump(void);
