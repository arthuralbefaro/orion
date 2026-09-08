#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile int int_count;
static volatile int alarm_count;
static volatile int term_count;

static void on_int(int sig)
{
    (void)sig;
    int_count++;
}

static void on_alarm(int sig)
{
    (void)sig;
    alarm_count++;
}

static void on_term(int sig)
{
    (void)sig;
    term_count++;
}

static int fails;

static void check(const char *what, int ok)
{
    printf("  %-40s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    signal(SIGINT, on_int);
    signal(SIGALRM, on_alarm);
    signal(SIGTERM, on_term);

    raise(SIGINT);
    check("handler de SIGINT rodou", int_count == 1);

    kill(getpid(), SIGINT);
    kill(getpid(), SIGTERM);
    check("dois sinais diferentes entregues", int_count == 2 && term_count == 1);

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGINT);
    check("sinal bloqueado nao e entregue", int_count == 2);
    sigprocmask(SIG_SETMASK, &old, NULL);
    check("sinal pendente entregue ao desbloquear", int_count == 3);

    signal(SIGINT, SIG_IGN);
    raise(SIGINT);
    check("SIG_IGN descarta o sinal", int_count == 3);
    signal(SIGINT, on_int);

    long t0 = now_ms();
    alarm(1);
    while (alarm_count == 0 && now_ms() - t0 < 4000)
        usleep(50000);
    long dt = now_ms() - t0;
    check("alarm(1) disparou SIGALRM", alarm_count == 1);
    check("alarm demorou perto de 1 s", dt >= 900 && dt < 2500);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        for (;;)
            usleep(50000);
    }
    usleep(200000);
    kill(pid, SIGTERM);
    int st = 0;
    waitpid(pid, &st, 0);
    check("filho morto por SIGTERM", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    check("WIFEXITED falso para morto por sinal", !WIFEXITED(st));

    pid = fork();
    if (pid == 0) {
        for (;;)
            usleep(50000);
    }
    usleep(200000);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    check("SIGKILL mata mesmo ignorado", WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

    pid = fork();
    if (pid == 0)
        _exit(7);
    waitpid(pid, &st, 0);
    check("saida normal codifica o codigo", WIFEXITED(st) && WEXITSTATUS(st) == 7);

    check("kill de pid inexistente da ESRCH", kill(31337, SIGTERM) < 0 && errno == ESRCH);

    printf("sigtest: %s\n", fails == 0 ? "TUDO OK" : "FALHOU");
    return fails ? 1 : 0;
}
