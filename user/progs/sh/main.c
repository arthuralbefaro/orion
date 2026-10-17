#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit.h"

#define MAX_ARGS    24
#define MAX_STAGES  8
#define MAX_ASSIGNS 8
#define MAX_JOBS    16
#define MAX_SEGS    16
#define JOB_TEXT    96

struct cmd {
    char *argv[MAX_ARGS];
    int argc;
    char *assign[MAX_ASSIGNS];
    int nassign;
    const char *in_file;
    const char *out_file;
    const char *err_file;
    int append;
};

struct job {
    int id;
    pid_t pgid;
    pid_t pids[MAX_STAGES];
    int status[MAX_STAGES];
    int n;
    int live;
    char text[JOB_TEXT];
};

enum seg_op { OP_END, OP_SEQ, OP_AND, OP_OR, OP_BG };

struct segment {
    char *text;
    enum seg_op op;
};

static volatile int interrupted;
static pid_t shell_pgid;
static int last_status;
static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

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

static int exit_code(int status)
{
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
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

static int name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
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
    const char *first = getenv("PATH");
    const char *sep = strchr(first, ':');
    int len = sep ? (int)(sep - first) : (int)strlen(first);
    snprintf(out, size, "%.*s/%s", len, first, cmd);
}

static void expand_vars(const char *in, char *out, size_t size)
{
    size_t o = 0;
    while (*in && o + 1 < size) {
        if (*in != '$') {
            out[o++] = *in++;
            continue;
        }
        in++;
        if (*in == '?') {
            in++;
            char num[16];
            snprintf(num, sizeof(num), "%d", last_status);
            for (const char *v = num; *v && o + 1 < size; v++)
                out[o++] = *v;
            continue;
        }
        char name[64];
        size_t n = 0;
        int braced = *in == '{';
        if (braced)
            in++;
        while (*in && n < sizeof(name) - 1 && name_char(*in))
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

static int is_assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    if (!eq || eq == word || eq - word >= 64)
        return 0;
    for (const char *p = word; p < eq; p++)
        if (!name_char(*p))
            return 0;
    return !(word[0] >= '0' && word[0] <= '9');
}

static void apply_assignment(const char *word)
{
    const char *eq = strchr(word, '=');
    char name[64];
    size_t n = (size_t)(eq - word);
    memcpy(name, word, n);
    name[n] = '\0';
    setenv(name, eq + 1, 1);
}

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
        } else if (c->argc == 0 && is_assignment(tok) && c->nassign < MAX_ASSIGNS) {
            c->assign[c->nassign++] = tok;
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

static void move_fd(int fd, int target)
{
    if (fd == target)
        return;
    dup2(fd, target);
    close(fd);
}

static void apply_redirects(const struct cmd *c)
{
    if (c->in_file) {
        int fd = open_redirect(c->in_file, O_RDONLY);
        if (fd < 0)
            _exit(1);
        move_fd(fd, STDIN_FILENO);
    }
    if (c->out_file) {
        int flags = O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC);
        int fd = open_redirect(c->out_file, flags);
        if (fd < 0)
            _exit(1);
        move_fd(fd, STDOUT_FILENO);
    }
    if (c->err_file) {
        int fd = open_redirect(c->err_file, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            _exit(1);
        move_fd(fd, STDERR_FILENO);
    }
}

static struct job *job_find(int id)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id && (id == 0 || jobs[i].id == id))
            return &jobs[i];
    return NULL;
}

static struct job *job_latest(void)
{
    struct job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].id && (!best || jobs[i].id > best->id))
            best = &jobs[i];
    return best;
}

static int job_arg(int argc, char **argv, struct job **out)
{
    if (argc < 2) {
        *out = job_latest();
        return *out ? 0 : -1;
    }
    const char *a = argv[1][0] == '%' ? argv[1] + 1 : argv[1];
    *out = job_find(atoi(a));
    return *out && atoi(a) > 0 ? 0 : -1;
}

static int job_finish(struct job *j, int quiet)
{
    int st = j->status[j->n - 1];
    if (!quiet)
        printf("[%d] pronto  %s\n", j->id, j->text);
    j->id = 0;
    return st;
}

static void job_poll(int quiet)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &jobs[i];
        if (!j->id)
            continue;
        for (int s = 0; s < j->n; s++) {
            if (!j->pids[s])
                continue;
            int st = 0;
            if (waitpid(j->pids[s], &st, WNOHANG) > 0) {
                j->status[s] = st;
                j->pids[s] = 0;
                j->live--;
            }
        }
        if (j->live == 0)
            job_finish(j, quiet);
    }
}

static int job_wait(struct job *j, int foreground)
{
    if (foreground)
        set_foreground(j->pgid);
    for (int s = 0; s < j->n; s++) {
        if (!j->pids[s])
            continue;
        int st = 0;
        while (waitpid(j->pids[s], &st, 0) < 0 && errno == EINTR)
            ;
        j->status[s] = st;
        j->pids[s] = 0;
        j->live--;
    }
    if (foreground)
        set_foreground(shell_pgid);
    return j->status[j->n - 1];
}

static void print_limit(const char *what, char flag, int resource)
{
    struct rlimit r;
    if (getrlimit(resource, &r) < 0)
        return;
    if (r.rlim_cur == RLIM_INFINITY)
        printf("-%c %-22s ilimitado\n", flag, what);
    else
        printf("-%c %-22s %lu\n", flag, what, (unsigned long)r.rlim_cur);
}

static int builtin_ulimit(int argc, char **argv)
{
    int resource = RLIMIT_NOFILE;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        print_limit("arquivos abertos", 'n', RLIMIT_NOFILE);
        print_limit("processos", 'u', RLIMIT_NPROC);
        print_limit("pilha (bytes)", 's', RLIMIT_STACK);
        return 0;
    }
    if (argc > 1 && argv[1][0] == '-') {
        if (strcmp(argv[1], "-n") == 0)
            resource = RLIMIT_NOFILE;
        else if (strcmp(argv[1], "-u") == 0)
            resource = RLIMIT_NPROC;
        else if (strcmp(argv[1], "-s") == 0)
            resource = RLIMIT_STACK;
        else {
            fprintf(stderr, "ulimit: opcao invalida %s (use -a, -n, -u ou -s)\n", argv[1]);
            return 2;
        }
        i = 2;
    }
    struct rlimit r;
    if (getrlimit(resource, &r) < 0)
        return 1;
    if (i >= argc) {
        if (r.rlim_cur == RLIM_INFINITY)
            puts("ilimitado");
        else
            printf("%lu\n", (unsigned long)r.rlim_cur);
        return 0;
    }
    r.rlim_cur = (rlim_t)strtoul(argv[i], NULL, 10);
    if (setrlimit(resource, &r) < 0) {
        fprintf(stderr, "ulimit: %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    return 0;
}

static int run_builtin(struct cmd *c)
{
    char **argv = c->argv;
    if (strcmp(argv[0], "exit") == 0)
        exit(c->argc > 1 ? atoi(argv[1]) : last_status);
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = c->argc > 1 ? argv[1] : "/";
        if (chdir(dir) < 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[256];
        if (!getcwd(buf, sizeof(buf)))
            return 1;
        puts(buf);
        return 0;
    }
    if (strcmp(argv[0], "sync") == 0) {
        sync();
        return 0;
    }
    if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < editor_history_count(); i++)
            printf("%4d  %s\n", i + 1, editor_history_at(i));
        return 0;
    }
    if (strcmp(argv[0], "export") == 0) {
        if (c->argc == 1) {
            for (char **e = environ; e && *e; e++)
                printf("export %s\n", *e);
            return 0;
        }
        for (int i = 1; i < c->argc; i++) {
            if (is_assignment(argv[i])) {
                apply_assignment(argv[i]);
            } else {
                const char *v = getenv(argv[i]);
                setenv(argv[i], v ? v : "", 1);
            }
        }
        return 0;
    }
    if (strcmp(argv[0], "unset") == 0) {
        for (int i = 1; i < c->argc; i++)
            unsetenv(argv[i]);
        return 0;
    }
    if (strcmp(argv[0], "clear") == 0) {
        fputs("\x1b[2J\x1b[H", stdout);
        return 0;
    }
    if (strcmp(argv[0], "ulimit") == 0)
        return builtin_ulimit(c->argc, argv);
    if (strcmp(argv[0], "jobs") == 0) {
        job_poll(0);
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].id)
                printf("[%d] rodando  %s\n", jobs[i].id, jobs[i].text);
        return 0;
    }
    if (strcmp(argv[0], "fg") == 0) {
        struct job *j;
        if (job_arg(c->argc, argv, &j) < 0) {
            fputs("fg: nenhum trabalho\n", stderr);
            return 1;
        }
        printf("%s\n", j->text);
        int st = job_wait(j, 1);
        j->id = 0;
        report(st);
        return exit_code(st);
    }
    if (strcmp(argv[0], "wait") == 0) {
        if (c->argc > 1) {
            struct job *j;
            if (job_arg(c->argc, argv, &j) < 0) {
                fprintf(stderr, "wait: %s: nenhum trabalho\n", argv[1]);
                return 127;
            }
            int st = job_wait(j, 0);
            job_finish(j, 0);
            return exit_code(st);
        }
        int st = 0;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].id) {
                job_wait(&jobs[i], 0);
                job_finish(&jobs[i], 0);
            }
        return st;
    }
    if (strcmp(argv[0], "help") == 0) {
        puts("builtins: cd, pwd, sync, clear, history, export, unset, jobs, fg, wait, ulimit, exit, help");
        puts("variaveis:   NOME=valor, $NOME, ${NOME}, $? (status do ultimo comando)");
        puts("             NOME=valor cmd  (so para esse comando); PATH escolhe onde procurar");
        puts("arquivos: ls cat cp mv rm mkdir touch head tail grep sort uniq tee wc echo");
        puts("sistema:  date sleep top ps kill uptime free time env printenv clear true false");
        puts("testes:   fstest polltest sigtest ptytest forktest badptr stress memtest crash");
        puts("rede:     ifconfig ping nslookup dhcp");
        puts("graficos: wm (compositor), fbtest, evtest");
        puts("redireciona: cmd > arq, cmd >> arq, cmd < arq, cmd 2> arq");
        puts("pipeline:    ls /bin | grep test | wc");
        puts("listas:      cmd1 ; cmd2   cmd1 && cmd2   cmd1 || cmd2   cmd &");
        puts("edicao:      setas, Home, End, Delete, Ctrl+L limpa, Ctrl+D sai");
        return 0;
    }
    return -1;
}

static int builtin_with_assigns(struct cmd *c)
{
    char saved[MAX_ASSIGNS][256];
    int had[MAX_ASSIGNS];
    for (int i = 0; i < c->nassign; i++) {
        const char *eq = strchr(c->assign[i], '=');
        char name[64];
        size_t n = (size_t)(eq - c->assign[i]);
        memcpy(name, c->assign[i], n);
        name[n] = '\0';
        const char *old = getenv(name);
        had[i] = old != NULL;
        if (old)
            snprintf(saved[i], sizeof(saved[i]), "%s", old);
        apply_assignment(c->assign[i]);
    }
    int r = run_builtin(c);
    for (int i = c->nassign - 1; i >= 0; i--) {
        const char *eq = strchr(c->assign[i], '=');
        char name[64];
        size_t n = (size_t)(eq - c->assign[i]);
        memcpy(name, c->assign[i], n);
        name[n] = '\0';
        if (had[i])
            setenv(name, saved[i], 1);
        else
            unsetenv(name);
    }
    return r;
}

static int run_pipeline(struct cmd *cmds, int n, int background, const char *text)
{
    pid_t pids[MAX_STAGES];
    pid_t pgid = 0;
    int prev_read = -1;
    int started = 0;

    for (int i = 0; i < n; i++) {
        int fds[2] = { -1, -1 };
        if (i + 1 < n && pipe(fds) < 0) {
            perror("sh: pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("sh: fork");
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }
        if (pid == 0) {
            setpgid(0, pgid);
            signal(SIGINT, SIG_DFL);
            if (prev_read >= 0)
                move_fd(prev_read, STDIN_FILENO);
            else if (background && !cmds[i].in_file) {
                int null = open("/dev/null", O_RDONLY);
                if (null >= 0)
                    move_fd(null, STDIN_FILENO);
            }
            if (fds[1] >= 0)
                move_fd(fds[1], STDOUT_FILENO);
            if (fds[0] >= 0)
                close(fds[0]);
            apply_redirects(&cmds[i]);
            for (int a = 0; a < cmds[i].nassign; a++)
                apply_assignment(cmds[i].assign[a]);
            char path[256];
            resolve(cmds[i].argv[0], path, sizeof(path));
            execv(path, cmds[i].argv);
            fprintf(stderr, "sh: %s: %s\n", cmds[i].argv[0], strerror(errno));
            _exit(127);
        }
        if (!pgid)
            pgid = pid;
        setpgid(pid, pgid);
        pids[started++] = pid;
        if (prev_read >= 0)
            close(prev_read);
        if (fds[1] >= 0)
            close(fds[1]);
        prev_read = fds[0];
    }
    if (prev_read >= 0)
        close(prev_read);
    if (!started)
        return 1;

    struct job tmp;
    struct job *j = &tmp;
    if (background) {
        j = NULL;
        for (int i = 0; i < MAX_JOBS; i++)
            if (!jobs[i].id) {
                j = &jobs[i];
                break;
            }
        if (!j) {
            fputs("sh: trabalhos demais em segundo plano; esperando este terminar\n", stderr);
            j = &tmp;
            background = 0;
        }
    }
    memset(j, 0, sizeof(*j));
    j->pgid = pgid;
    j->n = started;
    j->live = started;
    for (int i = 0; i < started; i++)
        j->pids[i] = pids[i];
    snprintf(j->text, sizeof(j->text), "%s", text);

    if (background) {
        j->id = next_job_id++;
        printf("[%d] %d\n", j->id, pgid);
        return 0;
    }
    int st = job_wait(j, 1);
    report(st);
    return exit_code(st);
}

static int run_segment(char *raw, int background)
{
    char line[LINE_SIZE * 2];
    expand_vars(raw, line, sizeof(line));

    char text[JOB_TEXT];
    const char *t = line;
    while (*t == ' ' || *t == '\t')
        t++;
    snprintf(text, sizeof(text), "%s", t);
    size_t tl = strlen(text);
    while (tl && (text[tl - 1] == ' ' || text[tl - 1] == '\t'))
        text[--tl] = '\0';

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
            return 2;
        if (argc == 0) {
            if (n > 1) {
                fputs("sh: sintaxe invalida\n", stderr);
                return 2;
            }
            for (int a = 0; a < cmds[count].nassign; a++)
                apply_assignment(cmds[count].assign[a]);
            return 0;
        }
        count++;
    }
    if (count == 0)
        return last_status;
    if (count == 1 && !background) {
        int r = cmds[0].nassign ? builtin_with_assigns(&cmds[0]) : run_builtin(&cmds[0]);
        if (r >= 0)
            return r;
    }
    return run_pipeline(cmds, count, background, text);
}

static int split_list(char *line, struct segment *segs)
{
    int n = 0;
    char *start = line;
    char *p = line;
    while (*p && n < MAX_SEGS) {
        enum seg_op op = OP_END;
        int len = 0;
        if (p[0] == '&' && p[1] == '&') {
            op = OP_AND;
            len = 2;
        } else if (p[0] == '|' && p[1] == '|') {
            op = OP_OR;
            len = 2;
        } else if (p[0] == ';') {
            op = OP_SEQ;
            len = 1;
        } else if (p[0] == '&' && !(p > line && (p[-1] == '>' || p[-1] == '<'))) {
            op = OP_BG;
            len = 1;
        }
        if (op == OP_END) {
            p++;
            continue;
        }
        *p = '\0';
        segs[n].text = start;
        segs[n].op = op;
        n++;
        p += len;
        start = p;
    }
    if (n < MAX_SEGS) {
        segs[n].text = start;
        segs[n].op = OP_END;
        n++;
    }
    return n;
}

static int blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t')
            return 0;
    return 1;
}

static void run_line(char *line)
{
    struct segment segs[MAX_SEGS];
    int n = split_list(line, segs);
    enum seg_op prev = OP_SEQ;
    for (int i = 0; i < n; i++) {
        if (blank(segs[i].text)) {
            if (segs[i].op != OP_END && segs[i].op != OP_SEQ) {
                fputs("sh: sintaxe invalida perto de '&' ou '|'\n", stderr);
                last_status = 2;
                return;
            }
            continue;
        }
        int skip = (prev == OP_AND && last_status != 0) || (prev == OP_OR && last_status == 0);
        if (!skip)
            last_status = run_segment(segs[i].text, segs[i].op == OP_BG);
        prev = segs[i].op;
    }
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
        job_poll(0);
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
