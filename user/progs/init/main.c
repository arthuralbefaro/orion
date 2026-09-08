#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("init: pid %d, iniciando /bin/sh\n", getpid());
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("init: fork");
            sleep(1);
            continue;
        }
        if (pid == 0) {
            char *args[] = { "sh", NULL };
            execv("/bin/sh", args);
            perror("init: execv /bin/sh");
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        printf("init: sh terminou (codigo %d), reiniciando\n", status);
        usleep(500000);
    }
}
