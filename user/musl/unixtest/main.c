#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
static char base[200];

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-52s ok\n", what);
    else
        printf("  %-52s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static int fails_with(int r, int err)
{
    return r < 0 && errno == err;
}

static socklen_t addr_of(struct sockaddr_un *a, const char *name)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof(a->sun_path), "%s/%s", base, name);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(a->sun_path) + 1);
}

static void pair(void)
{
    int sv[2];
    char buf[16] = { 0 };
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    check("dados de um lado para o outro", write(sv[0], "ping", 4) == 4 && read(sv[1], buf, sizeof(buf)) == 4 &&
                                               memcmp(buf, "ping", 4) == 0);
    check("e de volta", write(sv[1], "pong", 4) == 4 && read(sv[0], buf, sizeof(buf)) == 4 &&
                            memcmp(buf, "pong", 4) == 0);
    check("shutdown(SHUT_WR) da fim de arquivo do outro lado", shutdown(sv[0], SHUT_WR) == 0 &&
                                                                read(sv[1], buf, sizeof(buf)) == 0);
    check("depois do shutdown nao escreve mais", fails_with(write(sv[0], "x", 1), EPIPE));
    close(sv[1]);
    check("escrever para quem fechou da EPIPE", fails_with(write(sv[0], "x", 1), EPIPE));
    close(sv[0]);
    check("socketpair de AF_INET nao existe", fails_with(socketpair(AF_INET, SOCK_STREAM, 0, sv), EOPNOTSUPP));
}

static unsigned sum_of(const unsigned char *p, size_t n, unsigned s)
{
    for (size_t i = 0; i < n; i++)
        s = s * 31 + p[i];
    return s;
}

static void server(void)
{
    struct sockaddr_un a, got;
    socklen_t len = addr_of(&a, "ut.sock");
    unlink(a.sun_path);
    int l = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check("socket(AF_UNIX)", l >= 0);
    check("bind cria o no do socket", bind(l, (struct sockaddr *)&a, len) == 0);
    struct stat st;
    check("o no e S_IFSOCK", stat(a.sun_path, &st) == 0 && S_ISSOCK(st.st_mode));
    check("open no socket da ENXIO", fails_with(open(a.sun_path, O_RDONLY), ENXIO));
    int l2 = socket(AF_UNIX, SOCK_STREAM, 0);
    check("bind no mesmo caminho da EADDRINUSE", fails_with(bind(l2, (struct sockaddr *)&a, len), EADDRINUSE));
    close(l2);
    check("listen", listen(l, 4) == 0);
    struct pollfd pf = { l, POLLIN, 0 };
    check("listen sem ninguem nao fica legivel", poll(&pf, 1, 0) == 0);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    check("connect", connect(c, (struct sockaddr *)&a, len) == 0);
    check("quem escuta fica legivel", poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN));
    int s = accept(l, NULL, NULL);
    check("accept", s >= 0);
    socklen_t gl = sizeof(got);
    check("getpeername do cliente mostra o caminho", getpeername(c, (struct sockaddr *)&got, &gl) == 0 &&
                                                         strcmp(got.sun_path, a.sun_path) == 0);
    gl = sizeof(got);
    check("getsockname de quem escuta mostra o caminho", getsockname(l, (struct sockaddr *)&got, &gl) == 0 &&
                                                             strcmp(got.sun_path, a.sun_path) == 0);

    enum { TOTAL = 200000 };
    unsigned char *data = malloc(TOTAL);
    for (int i = 0; i < TOTAL; i++)
        data[i] = (unsigned char)(i * 7 + i / 251);
    pid_t pid = fork();
    if (pid == 0) {
        close(s);
        size_t done = 0;
        while (done < TOTAL) {
            ssize_t n = write(c, data + done, TOTAL - done < 7000 ? TOTAL - done : 7000);
            if (n <= 0)
                _exit(1);
            done += (size_t)n;
        }
        close(c);
        _exit(0);
    }
    close(c);
    unsigned char buf[4096];
    size_t got_bytes = 0;
    unsigned sum = 0;
    ssize_t n;
    while ((n = read(s, buf, sizeof(buf))) > 0) {
        sum = sum_of(buf, (size_t)n, sum);
        got_bytes += (size_t)n;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    check("200 KB de um processo filho, maior que o buffer", got_bytes == TOTAL && sum == sum_of(data, TOTAL, 0) &&
                                                                 WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("filho fechou: leitura da fim de arquivo", read(s, buf, sizeof(buf)) == 0);
    close(s);
    free(data);

    struct sockaddr_un other;
    socklen_t ol = addr_of(&other, "nao-existe.sock");
    int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    check("connect em caminho que nao existe da ENOENT", fails_with(connect(c2, (struct sockaddr *)&other, ol), ENOENT));
    ol = addr_of(&other, "arquivo-comum");
    int fd = open(other.sun_path, O_CREAT | O_WRONLY, 0644);
    close(fd);
    check("connect em arquivo comum da ECONNREFUSED", fails_with(connect(c2, (struct sockaddr *)&other, ol), ECONNREFUSED));
    unlink(other.sun_path);
    close(c2);
    check("unlink do caminho", unlink(a.sun_path) == 0);
    c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    check("depois do unlink ninguem conecta mais", fails_with(connect(c2, (struct sockaddr *)&a, len), ENOENT));
    close(c2);
    close(l);
    l = socket(AF_UNIX, SOCK_STREAM, 0);
    check("bind de novo no caminho livre", bind(l, (struct sockaddr *)&a, len) == 0 && listen(l, 1) == 0);
    c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    check("conecta no socket novo", connect(c2, (struct sockaddr *)&a, len) == 0);
    close(l);
    char b;
    check("quem escuta fechou: conexao pendente le fim", read(c2, &b, 1) == 0);
    close(c2);
    unlink(a.sun_path);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(base, sizeof(base), "%s", argc > 1 ? argv[1] : "/run");
    size_t n = strlen(base);
    while (n > 1 && base[n - 1] == '/')
        base[--n] = '\0';
    printf("\nunixtest: sockets AF_UNIX em %s\n", base);
    pair();
    server();
    printf("unixtest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
