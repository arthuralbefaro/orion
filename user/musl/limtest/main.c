#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static int open_until_full(int *last_errno)
{
    int fds[300];
    int n = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            *last_errno = errno;
            break;
        }
        fds[n++] = fd;
        if (n == 300)
            break;
    }
    for (int i = 0; i < n; i++)
        close(fds[i]);
    return n;
}

static int count_procs(void)
{
    struct sysinfo_orion si;
    if (orion_sysinfo(&si) < 0)
        return -1;
    return (int)si.procs;
}

int main(void)
{
    printf("limtest: limites de descritores e de processos\n");
    struct rlimit r;
    check("getrlimit(RLIMIT_NOFILE) = 256/256",
          getrlimit(RLIMIT_NOFILE, &r) == 0 && r.rlim_cur == 256 && r.rlim_max == 256);

    int err = 0;
    int n = open_until_full(&err);
    printf("  abriu %d descritores alem de 0, 1 e 2\n", n);
    check("253 descritores abertos alem de 0, 1 e 2", n == 253);
    check("o seguinte da EMFILE", err == EMFILE);

    r.rlim_cur = 16;
    r.rlim_max = 256;
    check("setrlimit(RLIMIT_NOFILE, 16)", setrlimit(RLIMIT_NOFILE, &r) == 0);
    err = 0;
    n = open_until_full(&err);
    check("com limite 16 abre so 13", n == 13 && err == EMFILE);
    check("dup2 para o fd 20 acima do limite da EBADF", dup2(0, 20) < 0 && errno == EBADF);

    r.rlim_cur = 256;
    check("volta o limite para 256", setrlimit(RLIMIT_NOFILE, &r) == 0);
    check("dup2 para o fd 255 funciona", dup2(0, 255) == 255);
    check("o fd 255 fecha", close(255) == 0);
    check("dup2 para o fd 256 da EBADF", dup2(0, 256) < 0 && errno == EBADF);

    r.rlim_cur = 300;
    r.rlim_max = 256;
    check("cur acima de max da EINVAL", setrlimit(RLIMIT_NOFILE, &r) < 0 && errno == EINVAL);
    r.rlim_cur = 512;
    r.rlim_max = 512;
    check("subir o max da EPERM", setrlimit(RLIMIT_NOFILE, &r) < 0 && errno == EPERM);

    fflush(stdout);
    int before = count_procs();
    struct rlimit np;
    getrlimit(RLIMIT_NPROC, &np);
    struct rlimit tight = { (rlim_t)before + 3, np.rlim_max };
    check("setrlimit(RLIMIT_NPROC, processos + 3)", setrlimit(RLIMIT_NPROC, &tight) == 0);
    pid_t kids[6];
    int made = 0, eagain = 0;
    for (int i = 0; i < 6; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            pause();
            _exit(0);
        }
        if (pid > 0)
            kids[made++] = pid;
        else if (errno == EAGAIN)
            eagain++;
    }
    check("so 3 forks passam, os outros dao EAGAIN", made == 3 && eagain == 3);
    for (int i = 0; i < made; i++)
        kill(kids[i], SIGTERM);
    for (int i = 0; i < made; i++)
        waitpid(kids[i], NULL, 0);
    setrlimit(RLIMIT_NPROC, &np);
    pid_t again = fork();
    if (again == 0)
        _exit(0);
    check("com o limite de volta, fork funciona", again > 0 && waitpid(again, NULL, 0) == again);

    printf("limtest: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
