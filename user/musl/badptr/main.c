#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define UNMAPPED  0x1000L
#define KERNEL    ((long)0xffffffff80000000ul)
#define NONCANON  ((long)0x8000000000000000ul)
#define TCGETS_N  0x5401
#define TIOCGWINSZ_N 0x5413
#define ARCH_GET_FS_N 0x1003

struct k_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

static int total, good;

static long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long r = syscall(n, a, b, c, d, e, f);
    return r < 0 ? -errno : r;
}

static long syscall3(long n, long a, long b, long c)
{
    return syscall6(n, a, b, c, 0, 0, 0);
}

static long syscall2(long n, long a, long b)
{
    return syscall6(n, a, b, 0, 0, 0, 0);
}

static long syscall1(long n, long a)
{
    return syscall6(n, a, 0, 0, 0, 0, 0);
}

static void expect_efault(const char *what, long r)
{
    total++;
    int ok = r == -EFAULT;
    if (ok)
        good++;
    printf("  %-44s %s", what, ok ? "EFAULT" : "FALHA");
    if (!ok)
        printf(" (devolveu %ld)", r);
    printf("\n");
}

static void expect_killed(const char *what, int (*fn)(void))
{
    total++;
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn());
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
    if (ok)
        good++;
    printf("  %-44s %s\n", what, ok ? "SIGSEGV" : "FALHA");
}

static int bad_sigreturn(void)
{
    __asm__ volatile("movq $0x1000, %%rsp\n\tmovq $15, %%rax\n\tsyscall" ::: "memory");
    return 0;
}

static int bad_handler(void)
{
    struct k_sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.handler = (unsigned long)NONCANON;
    sa.flags = SA_RESTORER;
    sa.restorer = (unsigned long)NONCANON;
    long r = syscall6(SYS_rt_sigaction, SIGTERM, (long)&sa, 0, 8, 0, 0);
    if (r == -EFAULT || r == -EINVAL)
        raise(SIGSEGV);
    raise(SIGTERM);
    return 0;
}

static volatile int nt_handled;

static void set_nt_handler(int sig)
{
    (void)sig;
    nt_handled = 1;
    __asm__ volatile("pushfq\n\torq $0x4000, (%%rsp)\n\tpopfq" ::: "memory", "cc");
}

static int nt_in_handler(void)
{
    signal(SIGTERM, set_nt_handler);
    raise(SIGTERM);
    return nt_handled ? 0 : 3;
}

static int ignored_segv(void)
{
    signal(SIGSEGV, SIG_IGN);
    *(volatile int *)UNMAPPED = 1;
    return 0;
}

static void expect_exit0(const char *what, int (*fn)(void))
{
    total++;
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn());
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (ok)
        good++;
    printf("  %-44s %s\n", what, ok ? "ok" : "FALHA");
}

int main(void)
{
    int fd = open("/dev/zero", O_RDONLY);
    int dir = open("/bin", O_RDONLY);
    printf("badptr: ponteiros invalidos em syscalls\n");

    expect_efault("read em endereco nao mapeado", syscall3(SYS_read, fd, UNMAPPED, 10));
    expect_efault("read em endereco do kernel", syscall3(SYS_read, fd, KERNEL, 10));
    expect_efault("read em endereco nao canonico", syscall3(SYS_read, fd, NONCANON, 10));
    expect_efault("read sobre o codigo (so leitura)", syscall3(SYS_read, fd, (long)&main, 10));
    expect_efault("write de endereco nao mapeado", syscall3(SYS_write, 1, UNMAPPED, 10));
    expect_efault("write de endereco do kernel", syscall3(SYS_write, 1, KERNEL, 10));
    expect_efault("open com caminho invalido", syscall2(SYS_open, UNMAPPED, 0));
    expect_efault("stat com caminho invalido", syscall2(SYS_stat, UNMAPPED, 0));
    expect_efault("stat com buffer invalido", syscall2(SYS_stat, (long)"/bin", UNMAPPED));
    expect_efault("fstat com buffer invalido", syscall2(SYS_fstat, fd, UNMAPPED));
    expect_efault("pipe com vetor invalido", syscall1(SYS_pipe, UNMAPPED));
    expect_efault("getcwd com buffer invalido", syscall2(SYS_getcwd, UNMAPPED, 100));
    expect_efault("clock_gettime com buffer invalido", syscall2(SYS_clock_gettime, 0, UNMAPPED));
    expect_efault("nanosleep com timespec invalido", syscall2(SYS_nanosleep, UNMAPPED, 0));
    expect_efault("execve com caminho invalido", syscall3(SYS_execve, UNMAPPED, 0, 0));
    expect_efault("execve com argv invalido", syscall3(SYS_execve, (long)"/bin/true", UNMAPPED, 0));
    char *argv_bad[] = { "true", (char *)UNMAPPED, NULL };
    expect_efault("execve com string de argv invalida", syscall3(SYS_execve, (long)"/bin/true", (long)argv_bad, 0));
    expect_efault("rt_sigaction com act invalido", syscall6(SYS_rt_sigaction, SIGTERM, UNMAPPED, 0, 8, 0, 0));
    expect_efault("rt_sigaction com old invalido", syscall6(SYS_rt_sigaction, SIGTERM, 0, KERNEL, 8, 0, 0));
    expect_efault("rt_sigprocmask com set invalido", syscall6(SYS_rt_sigprocmask, SIG_BLOCK, UNMAPPED, 0, 8, 0, 0));
    expect_efault("poll com vetor invalido", syscall3(SYS_poll, UNMAPPED, 1, 0));
    expect_efault("chdir com caminho invalido", syscall1(SYS_chdir, KERNEL));
    expect_efault("mkdir com caminho invalido", syscall2(SYS_mkdir, UNMAPPED, 0755));
    expect_efault("unlink com caminho invalido", syscall1(SYS_unlink, UNMAPPED));
    expect_efault("rename com caminho invalido", syscall2(SYS_rename, UNMAPPED, UNMAPPED));
    expect_efault("sysinfo com buffer invalido", syscall1(ORION_SYS_SYSINFO, UNMAPPED));
    expect_efault("proclist com buffer invalido", syscall2(ORION_SYS_PROCLIST, UNMAPPED, 4));
    expect_efault("readdir com buffer invalido", syscall3(ORION_SYS_READDIR, dir, UNMAPPED, 0));
    expect_efault("ioctl TCGETS com buffer invalido", syscall3(SYS_ioctl, 0, TCGETS_N, UNMAPPED));
    expect_efault("ioctl TIOCGWINSZ com buffer invalido", syscall3(SYS_ioctl, 0, TIOCGWINSZ_N, KERNEL));
    expect_efault("arch_prctl GET_FS com buffer invalido", syscall2(SYS_arch_prctl, ARCH_GET_FS_N, UNMAPPED));
    pid_t kid = fork();
    if (kid == 0)
        _exit(0);
    expect_efault("wait4 com status invalido", syscall6(SYS_wait4, kid, UNMAPPED, 0, 0, 0, 0));

    expect_killed("rt_sigreturn com pilha invalida", bad_sigreturn);
    expect_killed("handler de sinal nao canonico", bad_handler);
    expect_killed("SIGSEGV ignorado numa falta de pagina", ignored_segv);
    expect_exit0("flag NT ligada dentro do handler", nt_in_handler);

    close(fd);
    close(dir);
    printf("badptr: fim, %d/%d %s\n", good, total, good == total ? "TUDO OK" : "FALHA");
    return good == total ? 0 : 1;
}
