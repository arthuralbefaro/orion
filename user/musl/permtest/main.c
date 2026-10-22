#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static char base[200];

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-56s ok\n", what);
    else
        printf("  %-56s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static int fails_with(int r, int err)
{
    return r < 0 && errno == err;
}

static const char *P(const char *rel)
{
    static char bufs[6][300];
    static int n;
    char *b = bufs[n++ % 6];
    snprintf(b, 300, "%s/pt/%s", base, rel);
    return b;
}

static int put(const char *path, mode_t mode, const char *text)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? chmod(path, mode) : -1;
}

static int copy(const char *from, const char *to, mode_t mode)
{
    int in = open(from, O_RDONLY), out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0700);
    if (in < 0 || out < 0)
        return -1;
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n)
            return -1;
    close(in);
    close(out);
    return chmod(to, mode);
}

static int run_capture(const char *path, char *out, size_t size)
{
    int p[2];
    if (pipe(p) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *args[] = { (char *)path, NULL };
        execv(path, args);
        _exit(126);
    }
    close(p[1]);
    size_t got = 0;
    ssize_t n;
    while (got < size - 1 && (n = read(p[0], out + got, size - 1 - got)) > 0)
        got += (size_t)n;
    out[got] = 0;
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void as_user(void)
{
    struct stat st;
    char buf[256];
    check("getuid/geteuid 1000, getgid/getegid 100", getuid() == 1000 && geteuid() == 1000 && getgid() == 100 &&
                                                        getegid() == 100);
    check("usuario comum nao le o /etc/shadow", fails_with(open("/etc/shadow", O_RDONLY), EACCES));
    pid_t parent = getppid();
    check("setpgid nao muda o grupo do pai (root)", fails_with(setpgid(parent, getpid()), ESRCH) &&
                                                       getpgid(parent) != getpid());
    check("setpgid nao muda o grupo do init", fails_with(setpgid(1, getpid()), ESRCH) && getpgid(1) != getpid());
    check("setpgid nao entra num grupo que nao existe", fails_with(setpgid(0, 99999), EPERM));
    pid_t kid = fork();
    if (kid == 0) {
        pause();
        _exit(0);
    }
    check("setpgid muda o proprio grupo e o de um filho",
          setpgid(0, 0) == 0 && getpgid(0) == getpid() && setpgid(kid, kid) == 0 && getpgid(kid) == kid &&
              setpgid(kid, getpid()) == 0 && getpgid(kid) == getpid());
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);
    check("nao le arquivo 0600 de root", fails_with(open(P("segredo"), O_RDONLY), EACCES));
    int fd = open(P("publico"), O_RDONLY);
    check("le arquivo 0644 de root", fd >= 0 && read(fd, buf, sizeof(buf)) == 6);
    close(fd);
    check("nao escreve em arquivo 0644 de root", fails_with(open(P("publico"), O_WRONLY), EACCES));
    check("nao trunca arquivo de root", fails_with(truncate(P("publico"), 0), EACCES));
    check("nao cria arquivo em diretorio 0755 de root", fails_with(open(P("so-root/x"), O_CREAT | O_WRONLY, 0644), EACCES));
    check("nao cria diretorio la", fails_with(mkdir(P("so-root/d"), 0755), EACCES));
    check("nao apaga arquivo em diretorio de root", fails_with(unlink(P("publico")), EACCES));
    check("nao atravessa diretorio 0700 de root", fails_with(open(P("fechado/dentro"), O_RDONLY), EACCES));
    check("nao entra com chdir no 0700", fails_with(chdir(P("fechado")), EACCES));
    check("access: R_OK no 0600 da EACCES", fails_with(access(P("segredo"), R_OK), EACCES));
    check("access: R_OK no 0644 passa", access(P("publico"), R_OK) == 0);
    check("access: W_OK no 0644 de root da EACCES", fails_with(access(P("publico"), W_OK), EACCES));
    check("chmod em arquivo de outro da EPERM", fails_with(chmod(P("publico"), 0666), EPERM));
    check("chown da EPERM", fails_with(chown(P("publico"), 1000, 100), EPERM));
    struct timespec t[2] = { { 1, 0 }, { 1, 0 } };
    check("utimensat com hora explicita em arquivo de outro da EPERM",
          fails_with(utimensat(AT_FDCWD, P("publico"), t, 0), EPERM));
    check("utimensat com agora num arquivo que pode escrever", utimensat(AT_FDCWD, P("aberto"), NULL, 0) == 0);
    check("nao executa arquivo sem x", fails_with(execl(P("sem-x"), "sem-x", (char *)NULL), EACCES));

    check("cria no diretorio 1777", put(P("tmp/meu"), 0644, "meu\n") == 0);
    check("o arquivo novo e do usuario e do grupo dele", stat(P("tmp/meu"), &st) == 0 && st.st_uid == 1000 &&
                                                          st.st_gid == 100);
    check("chmod no proprio arquivo", chmod(P("tmp/meu"), 0600) == 0);
    check("chown so do grupo para um grupo dele", chown(P("tmp/meu"), (uid_t)-1, 100) == 0);
    check("chown para outro dono da EPERM", fails_with(chown(P("tmp/meu"), 0, (gid_t)-1), EPERM));
    check("chown para grupo que nao e dele da EPERM", fails_with(chown(P("tmp/meu"), (uid_t)-1, 0), EPERM));
    check("diretorio com setgid passa o grupo", put(P("grupo/novo"), 0644, "g\n") == 0 && stat(P("grupo/novo"), &st) == 0 &&
                                                 st.st_gid == 50);
    check("sticky: nao apaga arquivo de outro no 1777", fails_with(unlink(P("tmp/do-root")), EPERM));
    check("sticky: nao renomeia arquivo de outro", fails_with(rename(P("tmp/do-root"), P("tmp/roubado")), EPERM));
    check("sticky: apaga o proprio", unlink(P("tmp/meu")) == 0);
    check("kill no init da EPERM", fails_with(kill(1, 0), EPERM));
    check("setuid(0) da EPERM", fails_with(setuid(0), EPERM));
    check("setgroups da EPERM", fails_with(setgroups(0, NULL), EPERM));
    check("mount da EPERM", fails_with(mount("tmpfs", P("tmp"), "tmpfs", 0, NULL), EPERM));
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    check("bind em porta abaixo de 1024 da EACCES", fails_with(bind(s, (struct sockaddr *)&a, sizeof(a)), EACCES));
    close(s);
    char out[256];
    int rc = run_capture(P("id-suid"), out, sizeof(out));
    check("binario setuid de root roda com euid 0", rc == 0 && strstr(out, "uid=1000") && strstr(out, "euid=0"));
    rc = run_capture("/bin/id", out, sizeof(out));
    check("o id comum mostra uid 1000 sem euid", rc == 0 && strstr(out, "uid=1000") && !strstr(out, "euid="));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(base, sizeof(base), "%s", argc > 1 ? argv[1] : "/tmp");
    printf("\npermtest: permissoes em %s\n", base);
    if (getuid() != 0) {
        printf("permtest: precisa comecar como root\n");
        return 2;
    }
    mkdir(P(""), 0755);
    mkdir(P("so-root"), 0755);
    mkdir(P("fechado"), 0700);
    mkdir(P("tmp"), 0777);
    chmod(P("tmp"), 01777);
    mkdir(P("grupo"), 0777);
    chown(P("grupo"), 0, 50);
    chmod(P("grupo"), 02777);
    check("prepara os arquivos de root",
          put(P("segredo"), 0600, "segredo\n") == 0 && put(P("publico"), 0644, "aberto") == 0 &&
              put(P("aberto"), 0666, "x") == 0 && put(P("fechado/dentro"), 0644, "d") == 0 &&
              put(P("tmp/do-root"), 0666, "r") == 0 && put(P("sem-x"), 0644, "nao e programa") == 0 &&
              copy("/bin/id", P("id-suid"), 04755) == 0);
    struct stat st;
    check("chmod 4755 guarda o setuid", stat(P("id-suid"), &st) == 0 && (st.st_mode & 07777) == 04755);
    pid_t pid = fork();
    if (pid == 0) {
        fails = 0;
        if (setgroups(0, NULL) < 0 || setgid(100) < 0 || setuid(1000) < 0) {
            printf("permtest: nao virou usuario 1000 (errno %d)\n", errno);
            _exit(100);
        }
        as_user();
        _exit(fails > 99 ? 99 : fails);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check("o filho terminou como usuario sem falhas", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("root continua root no pai", getuid() == 0 && geteuid() == 0);
    static const char *const names[] = { "segredo", "publico", "aberto", "fechado/dentro", "tmp/do-root",
                                         "sem-x", "id-suid", "grupo/novo", NULL };
    for (int i = 0; names[i]; i++)
        unlink(P(names[i]));
    rmdir(P("fechado"));
    rmdir(P("so-root"));
    rmdir(P("tmp"));
    rmdir(P("grupo"));
    check("limpa o diretorio de teste", rmdir(P("")) == 0);
    printf("permtest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
