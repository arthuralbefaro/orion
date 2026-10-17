#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <orion.h>

#define AUTOSTART "/etc/autostart"
#define MAX_WORDS 8

static char shell_path[128] = "/bin/sh";
static char shell_args[MAX_WORDS][64];
static int  shell_argc;

static void split(char *line)
{
    shell_argc = 0;
    char *p = line;
    while (*p && shell_argc < MAX_WORDS) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
        snprintf(shell_args[shell_argc], sizeof(shell_args[0]), "%s", start);
        shell_argc++;
    }
}

static void run_once(const char *cmd)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        char line[256];
        snprintf(line, sizeof(line), "%s", cmd);
        split(line);
        char *argv[MAX_WORDS + 1];
        for (int i = 0; i < shell_argc; i++)
            argv[i] = shell_args[i];
        argv[shell_argc] = NULL;
        execv(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

static void read_autostart(void)
{
    FILE *f = fopen(AUTOSTART, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' ||
                     line[n - 1] == '\t'))
            line[--n] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            continue;
        char *eq = strchr(p, '=');
        char *space = strchr(p, ' ');
        if (eq && eq != p && (!space || eq < space)) {
            *eq = '\0';
            setenv(p, eq + 1, 1);
            *eq = '=';
            printf("init: ambiente: %s\n", p);
            continue;
        }
        if (strncmp(p, "shell ", 6) == 0) {
            snprintf(shell_path, sizeof(shell_path), "%s", p + 6);
            continue;
        }
        printf("init: autostart: %s\n", p);
        run_once(p);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("init: pid %d\n", getpid());
    read_autostart();
    printf("init: iniciando %s\n", shell_path);

    for (;;) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
            orion_perror("init: fork");
            sleep(1);
            continue;
        }
        if (pid == 0) {
            char line[128];
            snprintf(line, sizeof(line), "%s", shell_path);
            split(line);
            char *args[MAX_WORDS + 1];
            for (int i = 0; i < shell_argc; i++)
                args[i] = shell_args[i];
            args[shell_argc] = NULL;
            execv(args[0], args);
            orion_perror("init: execv");
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status))
            printf("init: %s morto por sinal %d, reiniciando\n", shell_path, WTERMSIG(status));
        else
            printf("init: %s terminou (codigo %d), reiniciando\n", shell_path,
                   WEXITSTATUS(status));
        usleep(500000);
    }
}
