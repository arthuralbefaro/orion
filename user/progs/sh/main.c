#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit.h"

#define MAX_ARGS   24
#define MAX_STAGES 8

struct cmd {
    char *argv[MAX_ARGS];
    int argc;
    const char *in_file;
    const char *out_file;
    const char *err_file;
    int append;
};

static volatile int interrupted;
static pid_t shell_pgid;

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void set_foreground(pid_t pgid)
{
    int32_t g = (int32_t)pgid;
    ioctl(STDIN_FILENO, TIOCSPGRP, &g);
}

static void report(int status)
{
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) != SIGINT)
            fprintf(stderr, "[morto por sinal %d: %s]\n", WTERMSIG(status),
                    strsignal(WTERMSIG(status)));
        else
            fprintf(stderr, "[morto por sinal %d]\n", WTERMSIG(status));
    } else if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[saiu com %d]\n", WEXITSTATUS(status));
    }
}

static void resolve(const char *cmd, char *out, size_t size)
{
    if (strchr(cmd, '/')) {
        snprintf(out, size, "%s", cmd);
        return;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) {
        snprintf(out, size, "/bin/%s", cmd);
        return;
    }
    while (*path) {
        const char *sep = strchr(path, ':');
        int len = sep ? (int)(sep - path) : (int)strlen(path);
        if (len > 0)
            snprintf(out, size, "%.*s/%s", len, path, cmd);
        else
            snprintf(out, size, "%s", cmd);
        struct stat st;
        if (stat(out, &st) == 0)
            return;
        if (!sep)
            break;
        path = sep + 1;
    }
    /* Nada no PATH: deixa o primeiro caminho para o erro sair com sentido. */
    const char *first = getenv("PATH");
    const char *sep = strchr(first, ':');
    int len = sep ? (int)(sep - first) : (int)strlen(first);
    snprintf(out, size, "%.*s/%s", len, first, cmd);
}

/* Troca $NOME e ${NOME} pelo valor da variavel. */
static void expand_vars(const char *in, char *out, size_t size)
{
    size_t o = 0;
    while (*in && o + 1 < size) {
        if (*in != '$') {
            out[o++] = *in++;
            continue;
        }
        in++;
        char name[64];
        size_t n = 0;
        int braced = *in == '{';
        if (braced)
            in++;
        while (*in && n < sizeof(name) - 1 &&
               ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') ||
                (*in >= '0' && *in <= '9') || *in == '_'))
            name[n++] = *in++;
        name[n] = '\0';
        if (braced && *in == '}')
            in++;
        if (!n) {
            out[o++] = '$';
            continue;
        }
        const char *val = getenv(name);
        if (!val)
            continue;
        while (*val && o + 1 < size)
            out[o++] = *val++;
    }
    out[o] = '\0';
}

/* "NOME=valor" no inicio da linha vira uma variavel de ambiente. */
static int assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    if (!eq || eq == word)
        return 0;
    for (const char *p = word; p < eq; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    char name[64];
    size_t n = (size_t)(eq - word);
    if (n >= sizeof(name))
        return 0;
    memcpy(name, word, n);
    name[n] = '\0';
    setenv(name, eq + 1, 1);
    return 1;
}

/* Separa a linha em tokens e retira os redirecionamentos. */
static int parse_cmd(char *text, struct cmd *c)
{
    memset(c, 0, sizeof(*c));
    char *save;
    char *tok = strtok_r(text, " \t", &save);
    while (tok) {
        const char **target = NULL;
        int append = 0;
        if (strcmp(tok, "<") == 0) {
            target = &c->in_file;
        } else if (strcmp(tok, ">") == 0) {
            target = &c->out_file;
        } else if (strcmp(tok, ">>") == 0) {
            target = &c->out_file;
            append = 1;
        } else if (strcmp(tok, "2>") == 0) {
            target = &c->err_file;
        } else if (tok[0] == '<' && tok[1]) {
            c->in_file = tok + 1;
        } else if (tok[0] == '>' && tok[1] == '>' && tok[2]) {
            c->out_file = tok + 2;
            c->append = 1;
        } else if (tok[0] == '>' && tok[1]) {
            c->out_file = tok + 1;
        } else if (c->argc < MAX_ARGS - 1) {
            c->argv[c->argc++] = tok;
        }
        if (target) {
            tok = strtok_r(NULL, " \t", &save);
            if (!tok) {
                fputs("sh: falta o nome do arquivo depois do redirecionamento\n", stderr);
                return -1;
            }
            *target = tok;
            if (append)
                c->append = 1;
        }
        tok = strtok_r(NULL, " \t", &save);
    }
    c->argv[c->argc] = NULL;
    return c->argc;
}

static int open_redirect(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0)
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
    return fd;
}

static void apply_redirects(const struct cmd *c)
{
    if (c->in_file) {
        int fd = open_redirect(c->in_file, O_RDONLY);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (c->out_file) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open_redirect(c->out_file, flags);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (c->err_file) {
        int fd = open_redirect(c->err_file, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            _exit(1);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
}

static int builtin(struct cmd *c)
{
    char **argv = c->argv;
    if (strcmp(argv[0], "exit") == 0)
        exit(c->argc > 1 ? atoi(argv[1]) : 0);
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = c->argc > 1 ? argv[1] : "/";
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
    if (strcmp(argv[0], "sync") == 0) {
        sync();
        return 1;
    }
    if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < editor_history_count(); i++)
            printf("%4d  %s\n", i + 1, editor_history_at(i));
        return 1;
    }
    if (strcmp(argv[0], "export") == 0) {
        if (c->argc == 1) {
            for (char **e = environ; e && *e; e++)
                printf("export %s\n", *e);
            return 1;
        }
        for (int i = 1; i < c->argc; i++)
            if (!assignment(argv[i])) {
                const char *v = getenv(argv[i]);
                setenv(argv[i], v ? v : "", 1);
            }
        return 1;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; i < c->argc; i++)
            unsetenv(argv[i]);
        return 1;
    }
    if (strcmp(argv[0], "clear") == 0) {
        fputs("\x1b[2J\x1b[H", stdout);
        return 1;
    }
    if (strcmp(argv[0], "help") == 0) {
        puts("builtins: cd, pwd, sync, clear, history, export, unset, exit, help");
        puts("variaveis:   NOME=valor, $NOME, ${NOME}; PATH escolhe onde procurar");
        puts("arquivos: ls cat cp mv rm mkdir touch head tail grep sort uniq tee wc echo");
        puts("sistema:  date sleep top env printenv clear hello memtest crash");
        puts("testes:   fstest polltest sigtest ptytest forktest");
        puts("rede:     ifconfig ping nslookup dhcp");
        puts("graficos: wm (compositor), fbtest, evtest");
        puts("redireciona: cmd > arq, cmd >> arq, cmd < arq, cmd 2> arq");
        puts("pipeline:    ls /bin | grep test | wc");
        puts("edicao:      setas, Home, End, Delete, Ctrl+L limpa, Ctrl+D sai");
        return 1;
    }
    return 0;
}

static void run_pipeline(struct cmd *cmds, int n)
{
    pid_t pids[MAX_STAGES];
    pid_t pgid = 0;
    int prev_read = -1;

    for (int i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < n && pipe(fds) < 0) {
            perror("sh: pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("sh: fork");
            break;
        }
        if (pid == 0) {
            setpgid(0, pgid);
            signal(SIGINT, SIG_DFL);
            if (prev_read >= 0) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], STDOUT_FILENO);
                close(fds[1]);
            }
            if (fds[0] >= 0)
                close(fds[0]);
            apply_redirects(&cmds[i]);
            char path[256];
            resolve(cmds[i].argv[0], path, sizeof(path));
            execv(path, cmds[i].argv);
            fprintf(stderr, "sh: %s: %s\n", cmds[i].argv[0], strerror(errno));
            _exit(127);
        }
        if (!pgid)
            pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        close(prev_read);

    if (!pgid)
        return;
    set_foreground(pgid);
    int status = 0;
    for (int i = 0; i < n; i++) {
        int st = 0;
        while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR)
            ;
        if (i == n - 1)
            status = st;
    }
    set_foreground(shell_pgid);
    report(status);
}

static void run_line(char *raw)
{
    char line[LINE_SIZE * 2];
    expand_vars(raw, line, sizeof(line));

    char first[128];
    size_t k = 0;
    const char *w = line;
    while (*w == ' ' || *w == '\t')
        w++;
    while (w[k] && w[k] != ' ' && w[k] != '\t' && k < sizeof(first) - 1) {
        first[k] = w[k];
        k++;
    }
    first[k] = '\0';
    if (k && !w[k] && assignment(first))
        return;

    char *stages[MAX_STAGES];
    int n = 0;
    char *p = line;
    while (n < MAX_STAGES) {
        stages[n++] = p;
        char *bar = strchr(p, '|');
        if (!bar)
            break;
        *bar = '\0';
        p = bar + 1;
    }

    struct cmd cmds[MAX_STAGES];
    int count = 0;
    for (int i = 0; i < n; i++) {
        int argc = parse_cmd(stages[i], &cmds[count]);
        if (argc < 0)
            return;
        if (argc == 0) {
            if (n > 1) {
                fputs("sh: sintaxe invalida\n", stderr);
                return;
            }
            continue;
        }
        count++;
    }
    if (count == 0)
        return;
    if (count == 1 && builtin(&cmds[0]))
        return;
    run_pipeline(cmds, count);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    setenv("PATH", "/bin", 0);
    setenv("HOME", "/", 0);
    setenv("SHELL", "/bin/sh", 0);
    char line[LINE_SIZE], cwd[256], prompt[300];

    setpgid(0, 0);
    shell_pgid = getpgrp();
    set_foreground(shell_pgid);
    signal(SIGINT, on_sigint);
    editor_init();
    printf("Orion sh (pid %d). Digite 'help'.\n", getpid());

    for (;;) {
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        snprintf(prompt, sizeof(prompt), "%s $ ", cwd);
        if (editor_readline(prompt, line, sizeof(line)) < 0) {
            if (interrupted) {
                interrupted = 0;
                stdin->err = 0;
                stdin->eof = 0;
                continue;
            }
            putchar('\n');
            return 0;
        }
        interrupted = 0;
        run_line(line);
    }
}
