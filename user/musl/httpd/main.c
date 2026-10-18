#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *root = "/";

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void reply(int c, int code, const char *what, const char *type, long long len)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.0 %d %s\r\nServer: orion-httpd\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                     "Connection: close\r\n\r\n",
                     code, what, type, len);
    write_all(c, head, (size_t)n);
}

static void send_text(int c, int code, const char *what, const char *body)
{
    reply(c, code, what, "text/plain", (long long)strlen(body));
    write_all(c, body, strlen(body));
}

static long long send_dir(int c, const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        send_text(c, 403, "Forbidden", "sem acesso\n");
        return -1;
    }
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    struct dirent *e;
    while ((e = readdir(d)))
        fprintf(m, "%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    closedir(d);
    fclose(m);
    reply(c, 200, "OK", "text/plain", (long long)len);
    write_all(c, buf, len);
    free(buf);
    return (long long)len;
}

static void serve(int c, const char *peer)
{
    char req[4096];
    size_t got = 0;
    while (got < sizeof(req) - 1) {
        ssize_t n = read(c, req + got, sizeof(req) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    req[got] = 0;
    char method[8] = "", path[1024] = "";
    if (sscanf(req, "%7s %1023s", method, path) != 2 || path[0] != '/') {
        send_text(c, 400, "Bad Request", "pedido invalido\n");
        printf("httpd: %s: pedido invalido\n", peer);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_text(c, 501, "Not Implemented", "so GET\n");
        printf("httpd: %s %s %s -> 501\n", peer, method, path);
        return;
    }
    if (strstr(path, "..")) {
        send_text(c, 403, "Forbidden", "caminho com ..\n");
        printf("httpd: %s GET %s -> 403\n", peer, path);
        return;
    }
    char full[2048];
    snprintf(full, sizeof(full), "%s%s", strcmp(root, "/") == 0 ? "" : root, path);
    struct stat st;
    if (stat(full, &st) < 0) {
        send_text(c, 404, "Not Found", "nao existe\n");
        printf("httpd: %s GET %s -> 404\n", peer, path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        long long n = send_dir(c, full);
        printf("httpd: %s GET %s -> 200 (diretorio, %lld bytes)\n", peer, path, n);
        return;
    }
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        send_text(c, 403, "Forbidden", "sem acesso\n");
        printf("httpd: %s GET %s -> 403\n", peer, path);
        return;
    }
    reply(c, 200, "OK", "application/octet-stream", (long long)st.st_size);
    char buf[16384];
    long long sent = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write_all(c, buf, (size_t)n) < 0)
            break;
        sent += n;
    }
    close(fd);
    printf("httpd: %s GET %s -> 200 (%lld bytes)\n", peer, path, sent);
}

int main(int argc, char **argv)
{
    int port = 80, limit = 0, idle = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            root = argv[++i];
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            idle = atoi(argv[++i]);
        else {
            fprintf(stderr, "uso: httpd [-p porta] [-r raiz] [-n conexoes] [-t segundos parado]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        fprintf(stderr, "httpd: socket: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
        fprintf(stderr, "httpd: porta %d: %s\n", port, strerror(errno));
        close(s);
        return 1;
    }
    socklen_t al = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &al);
    printf("httpd: servindo %s na porta %d\n", root, ntohs(a.sin_port));
    int served = 0;
    time_t last = time(NULL);
    while (!limit || served < limit) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv = { 1, 0 };
        int r = select(s + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) {
            fprintf(stderr, "httpd: select: %s\n", strerror(errno));
            break;
        }
        if (r <= 0) {
            if (idle && time(NULL) - last >= idle)
                break;
            continue;
        }
        struct sockaddr_in pa;
        socklen_t pl = sizeof(pa);
        int c = accept4(s, (struct sockaddr *)&pa, &pl, SOCK_CLOEXEC);
        if (c < 0) {
            fprintf(stderr, "httpd: accept: %s\n", strerror(errno));
            continue;
        }
        char peer[32];
        struct sockaddr_in pn;
        socklen_t pnl = sizeof(pn);
        if (getpeername(c, (struct sockaddr *)&pn, &pnl) == 0)
            snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(pn.sin_addr), ntohs(pn.sin_port));
        else
            snprintf(peer, sizeof(peer), "?");
        serve(c, peer);
        shutdown(c, SHUT_WR);
        char drain[512];
        while (read(c, drain, sizeof(drain)) > 0)
            ;
        close(c);
        served++;
        last = time(NULL);
    }
    close(s);
    printf("httpd: fim, %d conexao(oes)\n", served);
    return 0;
}
