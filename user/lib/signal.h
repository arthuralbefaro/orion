#pragma once

#include <sys/types.h>

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

typedef void (*sighandler_t)(int);
typedef unsigned long sigset_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SA_RESTART  0x10000000ul
#define SA_RESTORER 0x04000000ul

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct sigaction {
    unsigned long sa_handler;
    unsigned long sa_flags;
    unsigned long sa_restorer;
    unsigned long sa_mask;
};

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
sighandler_t signal(int sig, sighandler_t handler);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int kill(pid_t pid, int sig);
int raise(int sig);
unsigned alarm(unsigned seconds);
int pause(void);
const char *strsignal(int sig);
