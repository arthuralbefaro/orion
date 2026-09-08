#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

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
    if (strchr(cmd, '/'))
        snprintf(out, size, "%s", cmd);
    else
        snprintf(out, size, "/bin/%s", cmd);
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
    if (strcmp(argv[0], "help") == 0) {
        puts("builtins: cd, pwd, sync, exit, help");
        puts("arquivos: ls cat cp mv rm mkdir touch head tail grep sort uniq tee wc echo");
        puts("sistema:  date sleep hello memtest crash fstest polltest sigtest ptytest");
        puts("graficos: wm (compositor), fbtest, evtest");
        puts("redireciona: cmd > arq, cmd >> arq, cmd < arq, cmd 2> arq");
        puts("pipeline:    ls /bin | grep test | wc");
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

static void run_line(char *line)
{
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
    char line[512], cwd[256];

    setpgid(0, 0);
    shell_pgid = getpgrp();
    set_foreground(shell_pgid);
    signal(SIGINT, on_sigint);
    printf("Orion sh (pid %d). Digite 'help'.\n", getpid());

    for (;;) {
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        printf("%s $ ", cwd);
        if (!fgets(line, sizeof(line), stdin)) {
            if (interrupted) {
                interrupted = 0;
                stdin->err = 0;
                stdin->eof = 0;
                putchar('\n');
                continue;
            }
            putchar('\n');
            return 0;
        }
        interrupted = 0;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        run_line(line);
    }
}
