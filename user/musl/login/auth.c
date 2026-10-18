#define _GNU_SOURCE
#include "auth.h"

#include <crypt.h>
#include <grp.h>
#include <shadow.h>
#include <termios.h>

#define MAX_GROUPS 32

int auth_read_line(const char *prompt, char *buf, size_t size, int hide)
{
    struct termios old, quiet;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty && hide) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    fputs(prompt, stdout);
    fflush(stdout);
    size_t n = 0;
    int got = 0;
    for (;;) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0)
            break;
        got = 1;
        if (c == '\n' || c == '\r')
            break;
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = 0;
    if (tty && hide) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return got ? 0 : -1;
}

int auth_check(const char *user, const char *password)
{
    struct spwd *sp = getspnam(user);
    if (!sp || !sp->sp_pwdp)
        return 0;
    const char *hash = sp->sp_pwdp;
    if (hash[0] == '\0')
        return password[0] == '\0';
    if (hash[0] == '*' || hash[0] == '!')
        return 0;
    const char *got = crypt(password, hash);
    return got && strcmp(got, hash) == 0;
}

int auth_become(const struct passwd *pw)
{
    gid_t groups[MAX_GROUPS];
    int n = MAX_GROUPS;
    if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
        n = MAX_GROUPS;
    int unique = 0;
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < unique; j++)
            if (groups[j] == groups[i])
                seen = 1;
        if (!seen)
            groups[unique++] = groups[i];
    }
    if (setgroups((size_t)unique, groups) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
        return -1;
    return 0;
}

void auth_exec_shell(const struct passwd *pw, const char *command, int login)
{
    const char *shell = pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    if (!getenv("PATH"))
        setenv("PATH", "/bin", 1);
    if (login && chdir(pw->pw_dir) < 0 && chdir("/") < 0)
        fprintf(stderr, "sem diretorio %s\n", pw->pw_dir);
    if (command)
        execl(shell, shell, "-c", command, (char *)NULL);
    else
        execl(shell, shell, (char *)NULL);
    fprintf(stderr, "%s: %s\n", shell, strerror(errno));
    _exit(127);
}
