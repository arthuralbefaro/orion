#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define UNKNOWN_SYSCALL 777
#define CALLS 1000
#define CHILDREN 60

int main(void)
{
    int enosys = 0;
    for (int i = 0; i < CALLS; i++)
        if (syscall(UNKNOWN_SYSCALL, i, 0, 0) < 0)
            enosys++;
    int go[2];
    if (pipe(go) < 0) {
        perror("logflood: pipe");
        return 1;
    }
    pid_t pids[CHILDREN];
    int forked = 0;
    for (int i = 0; i < CHILDREN; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            char c;
            close(go[1]);
            while (read(go[0], &c, 1) > 0)
                ;
            kill(getpid(), SIGSEGV);
            _exit(0);
        }
        if (pids[i] > 0)
            forked++;
    }
    close(go[0]);
    close(go[1]);
    int killed = 0;
    for (int i = 0; i < CHILDREN; i++) {
        int st = 0;
        if (pids[i] > 0 && waitpid(pids[i], &st, 0) == pids[i] && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV)
            killed++;
    }
    printf("logflood: %d de %d syscalls desconhecidas deram ENOSYS, %d de %d filhos mortos por SIGSEGV juntos, fim\n",
           enosys, CALLS, killed, forked);
    return enosys == CALLS && killed == CHILDREN ? 0 : 1;
}
