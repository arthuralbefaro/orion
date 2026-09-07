#pragma once

#include <kernel/compiler.h>

#define PROCESS_NAME_MAX 32
#define USER_STACK_TOP   0x00007ffffffff000ull
#define USER_STACK_SIZE  (64 * 1024)

struct thread;

struct process {
    uint32_t pid;
    char name[PROCESS_NAME_MAX];
    uint64_t pml4_phys;
    uint64_t entry;
    uint64_t stack_top;
    uint64_t pages;
    int exit_code;
    struct thread *thread;
    struct process *next;
};

struct process *process_create_elf(const char *name, const void *image, size_t size);
struct process *process_current(void);
NORETURN void process_exit(int code);
void process_free(struct process *p);
void process_dump(void);