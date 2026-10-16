#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIDTH 8

static unsigned char area[16 * 4096];

static int child_work(int i)
{
    for (size_t k = 0; k < sizeof(area); k += 4096)
        area[k] = (unsigned char)(i + k);
    for (size_t k = 0; k < sizeof(area); k += 4096)
        if (area[k] != (unsigned char)(i + k))
            return 1;
    if (i % 7 == 0) {
        char *argv[] = { "true", NULL };
        execv("/bin/true", argv);
        return 2;
    }
    if (i % 5 == 0)
        sched_yield();
    return 0;
}

int main(int argc, char **argv)
{
    int total = argc > 1 ? atoi(argv[1]) : 2000;
    int width = argc > 2 ? atoi(argv[2]) : WIDTH;
    if (width < 1)
        width = 1;
    if (width > 32)
        width = 32;

    pid_t kids[32];
    int live = 0, started = 0, done = 0, bad = 0;
    memset(kids, 0, sizeof(kids));

    while (done < total) {
        while (live < width && started < total) {
            pid_t pid = fork();
            if (pid < 0) {
                printf("stress: fork falhou no %d\n", started);
                bad++;
                break;
            }
            if (pid == 0)
                _exit(child_work(started));
            for (int s = 0; s < width; s++)
                if (!kids[s]) {
                    kids[s] = pid;
                    break;
                }
            live++;
            started++;
        }
        int st = 0;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid <= 0) {
            printf("stress: waitpid devolveu %d com %d vivos\n", pid, live);
            bad++;
            break;
        }
        for (int s = 0; s < width; s++)
            if (kids[s] == pid)
                kids[s] = 0;
        live--;
        done++;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            bad++;
        if (done % 500 == 0)
            printf("stress: %d/%d\n", done, total);
    }
    printf("stress: %d processos, %d falha(s) %s\n", done, bad, bad ? "FALHA" : "ok");
    return bad ? 1 : 0;
}
