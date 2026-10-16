#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void run(char **argv)
{
    if (strchr(argv[0], '/')) {
        execv(argv[0], argv);
        return;
    }
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/bin";
    char full[256];
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        if (n && n + strlen(argv[0]) + 2 <= sizeof(full)) {
            memcpy(full, path, n);
            full[n] = '/';
            strcpy(full + n + 1, argv[0]);
            execv(full, argv);
        }
        if (!end)
            break;
        path = end + 1;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: time comando [argumentos...]\n");
        return 1;
    }

    uint64_t start = now_us();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "time: fork falhou\n");
        return 1;
    }
    if (pid == 0) {
        run(argv + 1);
        fprintf(stderr, "time: %s: comando nao encontrado\n", argv[1]);
        _exit(127);
    }

    signal(SIGINT, SIG_IGN);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    uint64_t us = now_us() - start;

    fprintf(stderr, "\nreal %lu.%03lu s", (unsigned long)(us / 1000000),
            (unsigned long)((us / 1000) % 1000));
    if (WIFSIGNALED(st))
        fprintf(stderr, "   (morto por sinal %d)", WTERMSIG(st));
    else if (WEXITSTATUS(st))
        fprintf(stderr, "   (saiu com %d)", WEXITSTATUS(st));
    fprintf(stderr, "\n");
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}