#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "edit.h"
#include <orion.h>

extern char **environ;

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
static int interactive = 1;
static int last_status;
static struct job jobs[MAX_JOBS];

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

static void set_foreground(pid_t pgid)
{
    if (!interactive)
        return;
    int32_t g = (int32_t)pgid;
    ioctl(STDIN_FILENO, TIOCSPGRP, &g);
}

static const char *const signal_names[32] = {
    [SIGHUP] = "Hangup", [SIGINT] = "Interrompido", [SIGQUIT] = "Quit",
    [SIGILL] = "Instrucao ilegal", [SIGABRT] = "Abortado", [SIGFPE] = "Erro aritmetico",
    [SIGKILL] = "Morto", [SIGSEGV] = "Falha de segmentacao", [SIGPIPE] = "Pipe quebrado",
    [SIGALRM] = "Alarme", [SIGTERM] = "Terminado", [SIGCHLD] = "Filho mudou de estado",
    [SIGUSR1] = "Sinal do usuario 1", [SIGUSR2] = "Sinal do usuario 2", [SIGBUS] = "Erro de barramento",
    [SIGTRAP] = "Armadilha", [SIGSYS] = "Chamada de sistema invalida", [SIGWINCH] = "Janela mudou",
};

static const char *signal_name(int sig)
{
    if (sig > 0 && sig < 32 && signal_names[sig])
        return signal_names[sig];
    return "Sinal desconhecido";
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
                    signal_name(WTERMSIG(status)));
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

static int quote_char(char c)
{
    return c == '\\' || c == '\'' || c == '"';
}

static char *skip_quoted(char *p)
{
    if (*p == '\\')
        return p[1] ? p + 2 : p + 1;
    if (*p == '\'') {
        char *e = strchr(p + 1, '\'');
        return e ? e + 1 : NULL;
    }
    for (p++; *p; p++) {
        if (*p == '\\' && p[1])
            p++;
        else if (*p == '"')
            return p + 1;
    }
    return NULL;
}

static int quotes_closed(char *line)
{
    for (char *p = line; *p;) {
        if (!quote_char(*p)) {
            p++;
            continue;
        }
        p = skip_quoted(p);
        if (!p)
            return 0;
    }
    return 1;
}

struct lex {
    char *out;
    char *end;
    char *field;
    int started;
    int overflow;
    char *fields[MAX_ARGS];
    int n;
};

static void lex_put(struct lex *l, char ch)
{
    l->started = 1;
    if (l->out + 1 < l->end)
        *l->out++ = ch;
    else
        l->overflow = 1;
}

static void lex_break(struct lex *l)
{
    if (!l->started)
        return;
    *l->out++ = '\0';
    if (l->n < MAX_ARGS)
        l->fields[l->n++] = l->field;
    l->field = l->out;
    l->started = 0;
}

static void lex_expand(struct lex *l, char **pp, int split)
{
    char *p = *pp;
    char num[16];
    const char *val = NULL;
    if (*p == '?' || *p == '$') {
        snprintf(num, sizeof(num), "%d", *p == '?' ? last_status : (int)getpid());
        val = num;
        p++;
    } else {
        char name[64];
        size_t n = 0;
        int braced = *p == '{';
        char *start = p;
        if (braced)
            p++;
        while (*p && n < sizeof(name) - 1 && name_char(*p))
            name[n++] = *p++;
        name[n] = '\0';
        if (braced && *p == '}')
            p++;
        if (!n) {
            p = start;
            lex_put(l, '$');
        } else {
            val = getenv(name);
        }
    }
    for (; val && *val; val++) {
        if (split && (*val == ' ' || *val == '\t' || *val == '\n'))
            lex_break(l);
        else
            lex_put(l, *val);
    }
    *pp = p;
}

static int word_end(char c)
{
    return !c || c == ' ' || c == '\t' || c == '<' || c == '>';
}

static int lex_word(struct lex *l, char **pp)
{
    char *p = *pp;
    int name = 1;
    int split = 1;
    int assign = 0;
    l->n = 0;
    l->field = l->out;
    l->started = 0;
    while (!word_end(*p)) {
        if (*p == '\\') {
            name = 0;
            p++;
            if (*p)
                lex_put(l, *p++);
        } else if (*p == '\'') {
            name = 0;
            l->started = 1;
            for (p++; *p && *p != '\''; p++)
                lex_put(l, *p);
            if (*p)
                p++;
        } else if (*p == '"') {
            name = 0;
            l->started = 1;
            for (p++; *p && *p != '"';) {
                if (*p == '\\' && (p[1] == '$' || p[1] == '"' || p[1] == '\\' || p[1] == '`')) {
                    lex_put(l, p[1]);
                    p += 2;
                } else if (*p == '$') {
                    p++;
                    lex_expand(l, &p, 0);
                } else {
                    lex_put(l, *p++);
                }
            }
            if (*p)
                p++;
        } else if (*p == '$') {
            name = 0;
            p++;
            lex_expand(l, &p, split);
        } else {
            if (*p == '=' && name && l->out > l->field && !(l->field[0] >= '0' && l->field[0] <= '9')) {
                assign = 1;
                split = 0;
            }
            if (!name_char(*p))
                name = 0;
            lex_put(l, *p++);
        }
    }
    lex_break(l);
    *pp = p;
    return assign;
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

static int parse_cmd(char *text, struct cmd *c, struct lex *l)
{
    memset(c, 0, sizeof(*c));
    const char **target = NULL;
    char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char **next = NULL;
        int len = 0;
        if (p[0] == '>' && p[1] == '>') {
            next = &c->out_file;
            c->append = 1;
            len = 2;
        } else if (p[0] == '>') {
            next = &c->out_file;
            c->append = 0;
            len = 1;
        } else if (p[0] == '<') {
            next = &c->in_file;
            len = 1;
        } else if (p[0] == '2' && p[1] == '>') {
            next = &c->err_file;
            len = 2;
        }
        if (next) {
            if (target)
                break;
            target = next;
            p += len;
            continue;
        }
        int assign = lex_word(l, &p);
        if (target) {
            if (l->n != 1) {
                fputs("sh: redirecionamento ambiguo\n", stderr);
                return -1;
            }
            *target = l->fields[0];
            target = NULL;
            continue;
        }
        for (int i = 0; i < l->n; i++) {
            if (assign && c->argc == 0 && c->nassign < MAX_ASSIGNS)
                c->assign[c->nassign++] = l->fields[i];
            else if (c->argc < MAX_ARGS - 1)
                c->argv[c->argc++] = l->fields[i];
        }
    }
    if (target) {
        fputs("sh: falta o nome do arquivo depois do redirecionamento\n", stderr);
        return -1;
    }
    if (l->overflow) {
        fputs("sh: comando longo demais\n", stderr);
        return -1;
    }
    c->argv[c->argc] = NULL;
    return c->argc;
}

static int open_redirect(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0)
        fprintf(stderr, "sh: %s: %s\n", path, orion_strerror(errno));
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
        fprintf(stderr, "ulimit: %s: %s\n", argv[i], orion_strerror(errno));
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
            fprintf(stderr, "cd: %s: %s\n", dir, orion_strerror(errno));
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
        puts("aspas:       'literal'  \"expande $NOME\"  \\x escapa um caractere");
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
            orion_perror("sh: pipe");
            break;
        }
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            orion_perror("sh: fork");
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }
        if (pid == 0) {
            if (interactive)
                setpgid(0, pgid);
            if (!interactive && background) {
                signal(SIGINT, SIG_IGN);
                signal(SIGQUIT, SIG_IGN);
            } else {
                signal(SIGINT, SIG_DFL);
            }
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
            fprintf(stderr, "sh: %s: %s\n", cmds[i].argv[0], orion_strerror(errno));
            _exit(127);
        }
        if (!pgid)
            pgid = pid;
        if (interactive)
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
        int top = 0;
        for (int i = 0; i < MAX_JOBS; i++)
            if (jobs[i].id > top)
                top = jobs[i].id;
        j->id = top + 1;
        if (interactive)
            printf("[%d] %d\n", j->id, pgid);
        return 0;
    }
    int st = job_wait(j, 1);
    if (interactive)
        report(st);
    return exit_code(st);
}

static int run_segment(char *raw, int background)
{
    char text[JOB_TEXT];
    const char *t = raw;
    while (*t == ' ' || *t == '\t')
        t++;
    snprintf(text, sizeof(text), "%s", t);
    size_t tl = strlen(text);
    while (tl && (text[tl - 1] == ' ' || text[tl - 1] == '\t'))
        text[--tl] = '\0';

    char *stages[MAX_STAGES];
    int n = 0;
    char *p = raw;
    stages[n++] = p;
    while (*p) {
        if (quote_char(*p)) {
            p = skip_quoted(p);
            continue;
        }
        if (*p == '|') {
            if (n == MAX_STAGES) {
                fputs("sh: pipeline longo demais\n", stderr);
                return 2;
            }
            *p++ = '\0';
            stages[n++] = p;
            continue;
        }
        p++;
    }

    static char words[LINE_SIZE * 4];
    struct lex lx;
    memset(&lx, 0, sizeof(lx));
    lx.out = words;
    lx.end = words + sizeof(words);
    struct cmd cmds[MAX_STAGES];
    int count = 0;
    for (int i = 0; i < n; i++) {
        int argc = parse_cmd(stages[i], &cmds[count], &lx);
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
        if (quote_char(*p)) {
            p = skip_quoted(p);
            continue;
        }
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
    if (!quotes_closed(line)) {
        fputs("sh: aspas sem fechar\n", stderr);
        last_status = 2;
        return;
    }
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
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("PATH", "/bin", 0);
    setenv("HOME", "/", 0);
    setenv("SHELL", "/bin/sh", 0);
    char line[LINE_SIZE], cwd[256], prompt[300];

    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        interactive = 0;
        if (strlen(argv[2]) >= sizeof(line)) {
            fputs("sh: comando longo demais para -c\n", stderr);
            return 2;
        }
        snprintf(line, sizeof(line), "%s", argv[2]);
        for (char *q = line; q && *q;) {
            if (quote_char(*q)) {
                q = skip_quoted(q);
                continue;
            }
            if (*q == '\n' || *q == '\r')
                *q = ';';
            q++;
        }
        run_line(line);
        return last_status;
    }

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
                clearerr(stdin);
                continue;
            }
            putchar('\n');
            return 0;
        }
        interrupted = 0;
        run_line(line);
    }
}
