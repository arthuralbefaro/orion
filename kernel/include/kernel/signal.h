#pragma once

#include <kernel/compiler.h>

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31
#define NSIG      65

#define SIG_DFL 0ull
#define SIG_IGN 1ull

#define SA_SIGINFO   0x00000004ull
#define SA_RESTORER  0x04000000ull
#define SA_ONSTACK   0x08000000ull
#define SA_RESTART   0x10000000ull
#define SA_NODEFER   0x40000000ull
#define SA_RESETHAND 0x80000000ull

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGMASK(s) (1ull << ((s) - 1))
#define SIG_UNBLOCKABLE (SIGMASK(SIGKILL) | SIGMASK(SIGSTOP))
#define SIG_TABLE_SIZE  ((NSIG - 1) * sizeof(struct user_sigaction))
#define SIGACT(p, s)    (&(p)->sig_handlers[(s) - 1])

#define SI_USER     0
#define SI_KERNEL   0x80
#define SI_TKILL    (-6)
#define CLD_EXITED  1
#define CLD_KILLED  2
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

struct user_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

struct user_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    uint64_t word[14];
};

struct sig_src {
    int32_t code;
    int32_t status;
    uint32_t pid;
    uint32_t pad;
    uint64_t addr;
};

struct process;
struct thread;
struct interrupt_frame;

void signal_init_process(struct process *p);
void signal_reset_on_exec(struct process *p);

int  signal_send(struct process *p, int sig);
int  signal_send_info(struct process *p, int sig, const struct sig_src *src);
int  signal_send_thread(struct thread *t, int sig);
int  signal_send_thread_info(struct thread *t, int sig, const struct sig_src *src, bool tlock_held);
void signal_retarget(struct process *p, uint64_t sigs);
uint64_t signal_pending_set(void);
long signal_suspend(uint64_t mask);
long signal_timedwait(uint64_t set, struct user_siginfo *info, bool has_timeout, uint64_t ms);
bool signal_pending_current(void);
void signal_check(struct interrupt_frame *f);

long signal_sigaction(int sig, const struct user_sigaction *act, struct user_sigaction *old);
long signal_procmask(int how, const uint64_t *set, uint64_t *old);
long signal_sigreturn(struct interrupt_frame *f);
void signal_copy_handlers(struct process *dst, struct process *src);

const char *signal_name(int sig);
