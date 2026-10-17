#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIDTH 8
#define MAX_WIDTH 256

static unsigned char area[16 * 4096];

static int hold_ms;

static int child_work(int i)
{
    for (size_t k = 0; k < sizeof(area); k += 4096)
        area[k] = (unsigned char)(i + k);
    for (size_t k = 0; k < sizeof(area); k += 4096)
        if (area[k] != (unsigned char)(i + k))
            return 1;
    if (hold_ms > 0)
        usleep((unsigned long)hold_ms * 1000);
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
    if (width > MAX_WIDTH)
        width = MAX_WIDTH;
    hold_ms = argc > 3 ? atoi(argv[3]) : 0;

    static pid_t kids[MAX_WIDTH];
    int live = 0, started = 0, done = 0, bad = 0, peak = 0;
    memset(kids, 0, sizeof(kids));

    pid_t me = getpid();
    while (done < total) {
        while (live < width && started < total) {
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "stress %d: fork falhou no %d (errno %d)\n", me, started, errno);
                bad++;
                break;
            }
            if (pid == 0) {
                if (getpid() == me) {
                    fprintf(stderr, "stress: fork devolveu 0 no proprio pai (pid %d)\n", me);
                    _exit(99);
                }
                _exit(child_work(started));
            }
            for (int s = 0; s < width; s++)
                if (!kids[s]) {
                    kids[s] = pid;
                    break;
                }
            live++;
            started++;
            if (live > peak)
                peak = live;
        }
        int st = 0;
        pid_t pid = waitpid(-1, &st, 0);
        if (pid <= 0) {
            fprintf(stderr, "stress %d: waitpid devolveu %d (errno %d) com %d vivos, %d feitos\n", me,
                    pid, errno, live, done);
            bad++;
            break;
        }
        int mine = 0;
        for (int s = 0; s < width; s++)
            if (kids[s] == pid) {
                kids[s] = 0;
                mine = 1;
            }
        if (!mine) {
            fprintf(stderr, "stress %d: waitpid devolveu %d, que nao e filho meu\n", me, pid);
            bad++;
        }
        live--;
        done++;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fprintf(stderr, "stress %d: filho %d terminou com status %x\n", me, pid, st);
            bad++;
        }
        if (done % 500 == 0)
            printf("stress: %d/%d\n", done, total);
    }
    if (hold_ms > 0)
        printf("stress: pico de %d vivos ao mesmo tempo\n", peak);
    printf("stress: %d processos, %d falha(s) %s\n", done, bad, bad ? "FALHA" : "ok");
    if (bad)
        fprintf(stderr, "stress %d: %d processos, %d falha(s) FALHA\n", me, done, bad);
    return bad ? 1 : 0;
}
