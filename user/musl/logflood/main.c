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
    int killed = 0;
    for (int i = 0; i < CHILDREN; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            kill(getpid(), SIGSEGV);
            _exit(0);
        }
        int st = 0;
        if (pid > 0 && waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV)
            killed++;
    }
    printf("logflood: %d de %d syscalls desconhecidas deram ENOSYS, %d de %d filhos mortos por SIGSEGV, fim\n",
           enosys, CALLS, killed, CHILDREN);
    return enosys == CALLS && killed == CHILDREN ? 0 : 1;
}
