#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct signame {
    const char *name;
    int num;
};

static const struct signame names[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT }, { "ILL", SIGILL },
    { "ABRT", SIGABRT }, { "FPE", SIGFPE },   { "KILL", SIGKILL }, { "SEGV", SIGSEGV },
    { "PIPE", SIGPIPE }, { "ALRM", SIGALRM }, { "TERM", SIGTERM }, { "CHLD", SIGCHLD },
};

static int parse_signal(const char *s)
{
    if (s[0] >= '0' && s[0] <= '9') {
        int n = atoi(s);
        return n > 0 && n < NSIG ? n : -1;
    }
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(s, names[i].name) == 0)
            return names[i].num;
    return -1;
}

static void list_signals(void)
{
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        printf("%2d %-5s %s\n", names[i].num, names[i].name, strsignal(names[i].num));
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int i = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        list_signals();
        return 0;
    }
    if (argc > 2 && argv[1][0] == '-' && strcmp(argv[1], "--") != 0) {
        sig = parse_signal(argv[1] + 1);
        if (sig < 0) {
            fprintf(stderr, "kill: sinal invalido: %s (use kill -l)\n", argv[1] + 1);
            return 1;
        }
        i = 2;
    }
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    if (i >= argc) {
        fprintf(stderr, "uso: kill [-SINAL] pid...   (pid negativo = grupo; kill -l lista)\n");
        return 1;
    }

    int fails = 0;
    for (; i < argc; i++) {
        char *end;
        long pid = strtol(argv[i], &end, 10);
        if (*end || pid == 0) {
            fprintf(stderr, "kill: pid invalido: %s\n", argv[i]);
            fails++;
            continue;
        }
        if (kill((pid_t)pid, sig) < 0) {
            fprintf(stderr, "kill: %ld: processo nao encontrado\n", pid);
            fails++;
        }
    }
    return fails ? 1 : 0;
}
