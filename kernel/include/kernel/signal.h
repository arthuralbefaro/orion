#pragma once

#include <kernel/compiler.h>

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define NSIG     32

#define SIG_DFL 0ull
#define SIG_IGN 1ull

#define SA_RESTART  0x10000000ull
#define SA_RESTORER 0x04000000ull

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGMASK(s) (1ull << (s))

struct user_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

struct process;
struct interrupt_frame;

void signal_init_process(struct process *p);
void signal_reset_on_exec(struct process *p);

int  signal_send(struct process *p, int sig);
bool signal_pending_current(void);
void signal_check(struct interrupt_frame *f);

long signal_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old);
long signal_procmask(int how, const uint64_t *set, uint64_t *old);
long signal_sigreturn(struct interrupt_frame *f);

const char *signal_name(int sig);
