#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void drain(int master, const char *tag)
{
    char buf[256];
    for (int tries = 0; tries < 40; tries++) {
        long n = read(master, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("%s%s", tag, buf);
            fflush(stdout);
            tries = 0;
            continue;
        }
        if (n == 0)
            return;
        usleep(50000);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    int master, slave;
    if (orion_openpty(&master, &slave) < 0) {
        fprintf(stderr, "ptytest: openpty: %s\n", orion_strerror(errno));
        return 1;
    }
    printf("ptytest: master=%d slave=%d\n", master, slave);

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        orion_perror("ptytest: fork");
        return 1;
    }
    if (pid == 0) {
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);
        char *args[] = { "sh", NULL };
        execv("/bin/sh", args);
        _exit(127);
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);

    usleep(300000);
    drain(master, "");

    const char *cmd = "echo ola\n";
    write(master, cmd, strlen(cmd));
    usleep(400000);
    drain(master, "");

    const char *bye = "exit\n";
    write(master, bye, strlen(bye));
    drain(master, "");

    int status = 0;
    waitpid(pid, &status, 0);
    printf("\nptytest: sh saiu com %d\n", status);
    close(master);
    return 0;
}
