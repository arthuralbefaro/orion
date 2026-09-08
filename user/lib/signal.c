#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

__attribute__((naked)) static void sigreturn_trampoline(void)
{
    __asm__ volatile("movq $15, %rax\n\t"
                     "syscall\n\t"
                     "ud2");
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct sigaction tmp;
    const struct sigaction *use = act;
    if (act) {
        tmp = *act;
        tmp.sa_restorer = (unsigned long)sigreturn_trampoline;
        tmp.sa_flags |= SA_RESTORER;
        use = &tmp;
    }
    return (int)__syscall_ret(syscall3(SYS_RT_SIGACTION, sig, (long)use, (long)old));
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act, old;
    memset(&act, 0, sizeof(act));
    act.sa_handler = (unsigned long)handler;
    act.sa_flags = SA_RESTART;
    if (sigaction(sig, &act, &old) < 0)
        return SIG_ERR;
    return (sighandler_t)old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)__syscall_ret(syscall3(SYS_RT_SIGPROCMASK, how, (long)set, (long)old));
}

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    *set = ~0ul;
    return 0;
}

int sigaddset(sigset_t *set, int sig)
{
    if (sig <= 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    *set |= 1ul << sig;
    return 0;
}

int kill(pid_t pid, int sig)
{
    return (int)__syscall_ret(syscall2(SYS_KILL, pid, sig));
}

int raise(int sig)
{
    return kill(getpid(), sig);
}

unsigned alarm(unsigned seconds)
{
    return (unsigned)syscall1(SYS_ALARM, seconds);
}

int pause(void)
{
    for (;;)
        usleep(50000);
}

static const char *const signames[NSIG] = {
    [SIGHUP] = "Hangup", [SIGINT] = "Interrompido", [SIGQUIT] = "Quit",
    [SIGILL] = "Instrucao ilegal", [SIGABRT] = "Abortado", [SIGFPE] = "Erro aritmetico",
    [SIGKILL] = "Morto", [SIGSEGV] = "Falha de segmentacao", [SIGPIPE] = "Pipe quebrado",
    [SIGALRM] = "Alarme", [SIGTERM] = "Terminado", [SIGCHLD] = "Filho mudou de estado",
};

const char *strsignal(int sig)
{
    if (sig > 0 && sig < NSIG && signames[sig])
        return signames[sig];
    return "Sinal desconhecido";
}
