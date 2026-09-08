#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#define MAX_ARGS 16

static int split(char *line, char **argv)
{
    int argc = 0;
    char *save;
    for (char *tok = strtok_r(line, " \t", &save); tok && argc < MAX_ARGS - 1; tok = strtok_r(NULL, " \t", &save))
        argv[argc++] = tok;
    argv[argc] = NULL;
    return argc;
}

static void resolve(const char *cmd, char *out, size_t size)
{
    if (strchr(cmd, '/'))
        snprintf(out, size, "%s", cmd);
    else
        snprintf(out, size, "/bin/%s", cmd);
}

static pid_t spawn(char **argv, int in_fd, int out_fd, int extra_close)
{
    char path[256];
    resolve(argv[0], path, sizeof(path));
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (in_fd != STDIN_FILENO) { dup2(in_fd, STDIN_FILENO); close(in_fd); }
        if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
        if (extra_close >= 0)
            close(extra_close);
        execv(path, argv);
        fprintf(stderr, "sh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

static int builtin(int argc, char **argv)
{
    if (strcmp(argv[0], "exit") == 0)
        exit(argc > 1 ? atoi(argv[1]) : 0);
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : "/";
        if (chdir(dir) < 0)
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[256];
        if (getcwd(buf, sizeof(buf)))
            puts(buf);
        return 1;
    }
    if (strcmp(argv[0], "help") == 0) {
        puts("builtins: cd, pwd, exit, help");
        puts("programas em /bin: echo cat ls hello date wc memtest crash");
        puts("pipe simples: ls | wc");
        return 1;
    }
    return 0;
}

static void run_line(char *line)
{
    char *bar = strchr(line, '|');
    char *argv1[MAX_ARGS], *argv2[MAX_ARGS];

    if (bar) {
        *bar = '\0';
        if (split(line, argv1) == 0 || split(bar + 1, argv2) == 0) {
            fputs("sh: sintaxe invalida\n", stderr);
            return;
        }
        int fds[2];
        if (pipe(fds) < 0) {
            perror("sh: pipe");
            return;
        }
        pid_t p1 = spawn(argv1, STDIN_FILENO, fds[1], fds[0]);
        pid_t p2 = spawn(argv2, fds[0], STDOUT_FILENO, fds[1]);
        close(fds[0]);
        close(fds[1]);
        int st;
        if (p1 > 0) waitpid(p1, &st, 0);
        if (p2 > 0) waitpid(p2, &st, 0);
        return;
    }

    int argc = split(line, argv1);
    if (argc == 0 || builtin(argc, argv1))
        return;

    pid_t pid = spawn(argv1, STDIN_FILENO, STDOUT_FILENO, -1);
    if (pid < 0) {
        perror("sh: fork");
        return;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0)
        fprintf(stderr, "[saiu com %d]\n", status);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[256], cwd[256];
    printf("Orion sh (pid %d). Digite 'help'.\n", getpid());
    for (;;) {
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        printf("%s $ ", cwd);
        if (!fgets(line, sizeof(line), stdin))
            continue;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        run_line(line);
    }
}
