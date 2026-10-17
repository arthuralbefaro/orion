#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <syscall.h>
#include <termios.h>
#include <unistd.h>

#define SYS_READV_N      19
#define SYS_WRITEV_N     20
#define SYS_UNAME_N      63
#define SYS_GETUID_N     102
#define SYS_GETGID_N     104
#define SYS_GETEUID_N    107
#define SYS_GETEGID_N    108
#define SYS_SIGALTSTACK_N 131
#define SYS_GETTID_N     186
#define SYS_SET_TID_N    218
#define SYS_EXIT_GROUP_N 231

struct iov {
    void *base;
    unsigned long len;
};

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct stack_t {
    void *sp;
    int flags;
    int pad;
    unsigned long size;
};

static int fails;
static volatile int usr1_count;
static volatile int rt_count;

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static void on_usr1(int sig)
{
    (void)sig;
    usr1_count++;
}

static void on_rt(int sig)
{
    (void)sig;
    rt_count++;
}

int main(void)
{
    printf("sctest: syscalls que a musl chama na partida\n");

    int tid_word = 123;
    long tid = syscall1(SYS_SET_TID_N, (long)&tid_word);
    check("set_tid_address devolve o tid", tid == getpid());
    check("gettid = getpid numa thread so", syscall0(SYS_GETTID_N) == getpid());
    check("getuid/geteuid/getgid/getegid = 0",
          syscall0(SYS_GETUID_N) == 0 && syscall0(SYS_GETEUID_N) == 0 && syscall0(SYS_GETGID_N) == 0 &&
              syscall0(SYS_GETEGID_N) == 0);

    struct utsname u;
    memset(&u, 0, sizeof(u));
    check("uname", syscall1(SYS_UNAME_N, (long)&u) == 0);
    check("uname: sysname Orion, machine x86_64",
          strcmp(u.sysname, "Orion") == 0 && strcmp(u.machine, "x86_64") == 0);
    printf("  uname: %s %s %s %s\n", u.sysname, u.nodename, u.release, u.machine);

    int fds[2];
    pipe(fds);
    char a[] = "um ", b[] = "dois ", c[] = "tres";
    struct iov w[3] = { { a, 3 }, { b, 5 }, { c, 4 } };
    check("writev junta 3 pedacos", syscall3(SYS_WRITEV_N, fds[1], (long)w, 3) == 12);
    char x[5], y[16];
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
    struct iov r[2] = { { x, 4 }, { y, 15 } };
    long n = syscall3(SYS_READV_N, fds[0], (long)r, 2);
    check("readv espalha em 2 buffers", n == 12 && memcmp(x, "um d", 4) == 0 && strcmp(y, "ois tres") == 0);
    close(fds[0]);
    close(fds[1]);
    check("writev com 0 pedacos devolve 0", syscall3(SYS_WRITEV_N, 1, (long)w, 0) == 0);
    check("writev com iovec invalido da EFAULT", syscall3(SYS_WRITEV_N, 1, 0x1000, 1) == -EFAULT);

    signal(SIGUSR1, on_usr1);
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    check("sigaddset usa o bit sig-1 do Linux", set == (1ul << (SIGUSR1 - 1)));
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    check("SIGUSR1 bloqueado nao e entregue", usr1_count == 0);
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    check("a mascara lida de volta tem o SIGUSR1", sigismember(&cur, SIGUSR1) == 1);
    sigprocmask(SIG_SETMASK, &old, NULL);
    check("desbloquear entrega o SIGUSR1", usr1_count == 1);

    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_SETMASK, &all, &old);
    sigprocmask(SIG_SETMASK, NULL, &cur);
    check("SIGKILL e SIGSTOP nunca ficam bloqueados",
          !sigismember(&cur, SIGKILL) && !sigismember(&cur, SIGSTOP) && sigismember(&cur, SIGRTMAX));
    sigprocmask(SIG_SETMASK, &old, NULL);

    long bad = syscall6(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)&set, 0, 4, 0, 0);
    check("rt_sigprocmask com sigsetsize 4 da EINVAL", bad == -EINVAL);

    signal(SIGRTMIN + 6, on_rt);
    raise(SIGRTMIN + 6);
    check("sinal de tempo real 40 entregue ao handler", rt_count == 1);
    check("sigaction em SIGSTOP da EINVAL", signal(SIGSTOP, on_rt) == SIG_ERR);

    struct stack_t ss;
    memset(&ss, 0xff, sizeof(ss));
    check("sigaltstack devolve SS_DISABLE", syscall2(SYS_SIGALTSTACK_N, 0, (long)&ss) == 0 && ss.flags == 2);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    check("ioctl TIOCGWINSZ no terminal", ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0);
    struct termios t;
    memset(&t, 0, sizeof(t));
    check("TCGETS no formato do Linux", ioctl(0, TCGETS, &t) == 0 && (t.c_cflag & 0x30) == 0x30 &&
                                             t.c_cc[0] == 3 && t.c_cc[4] == 4);
    check("struct termios tem 36 bytes", sizeof(struct termios) == 36);

    pid_t pid = fork();
    if (pid == 0) {
        int word = 77;
        syscall1(SYS_SET_TID_N, (long)&word);
        syscall1(SYS_EXIT_GROUP_N, 7);
        _exit(1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check("exit_group termina com o codigo pedido", WIFEXITED(st) && WEXITSTATUS(st) == 7);

    printf("sctest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
