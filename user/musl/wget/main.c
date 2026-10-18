#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_REDIRECTS 5

struct url {
    char host[256];
    int port;
    char path[1024];
};

static int quiet;

static int parse_url(const char *text, struct url *u)
{
    if (strncmp(text, "http://", 7) != 0) {
        fprintf(stderr, "wget: so http:// e suportado: %s\n", text);
        return -1;
    }
    const char *h = text + 7;
    size_t hl = strcspn(h, ":/");
    if (hl == 0 || hl >= sizeof(u->host)) {
        fprintf(stderr, "wget: host invalido em %s\n", text);
        return -1;
    }
    memcpy(u->host, h, hl);
    u->host[hl] = 0;
    const char *rest = h + hl;
    u->port = 80;
    if (*rest == ':') {
        u->port = atoi(rest + 1);
        rest += 1 + strspn(rest + 1, "0123456789");
        if (u->port <= 0 || u->port > 65535) {
            fprintf(stderr, "wget: porta invalida em %s\n", text);
            return -1;
        }
    }
    snprintf(u->path, sizeof(u->path), "%s", *rest ? rest : "/");
    return 0;
}

static int connect_to(const struct url *u)
{
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof(port), "%d", u->port);
    int r = getaddrinfo(u->host, port, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "wget: %s: %s\n", u->host, gai_strerror(r));
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || connect(s, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "wget: conectando em %s:%d: %s\n", u->host, u->port, strerror(errno));
        freeaddrinfo(res);
        if (s >= 0)
            close(s);
        return -1;
    }
    if (!quiet) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
        fprintf(stderr, "wget: conectado a %s (%s):%d\n", u->host, ip, u->port);
    }
    freeaddrinfo(res);
    return s;
}

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

static const char *default_name(const char *path)
{
    static char name[256];
    const char *q = strchr(path, '?');
    size_t len = q ? (size_t)(q - path) : strlen(path);
    const char *slash = path;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/')
            slash = path + i + 1;
    size_t n = len - (size_t)(slash - path);
    if (n == 0 || n >= sizeof(name))
        return "index.html";
    memcpy(name, slash, n);
    name[n] = 0;
    return name;
}

static const char *header_value(char *head, const char *key)
{
    size_t kl = strlen(key);
    for (char *line = strstr(head, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char *l = line + 2;
        if (strncasecmp(l, key, kl) == 0 && l[kl] == ':') {
            char *v = l + kl + 1;
            while (*v == ' ')
                v++;
            return v;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *out_name = NULL;
    const char *url_text = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
            out_name = argv[++i];
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else if (argv[i][0] != '-' && !url_text)
            url_text = argv[i];
        else {
            fprintf(stderr, "uso: wget [-q] [-O arquivo] http://host[:porta]/caminho\n");
            return 2;
        }
    }
    if (!url_text) {
        fprintf(stderr, "uso: wget [-q] [-O arquivo] http://host[:porta]/caminho\n");
        return 2;
    }
    struct url u;
    if (parse_url(url_text, &u) < 0)
        return 1;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int s = connect_to(&u);
        if (s < 0)
            return 1;
        char req[1536];
        int rl = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: orion-wget\r\nAccept: */*\r\n"
                          "Connection: close\r\n\r\n",
                          u.path, u.host);
        if (write_all(s, req, (size_t)rl) < 0) {
            fprintf(stderr, "wget: envio: %s\n", strerror(errno));
            close(s);
            return 1;
        }
        char head[8192];
        size_t hl = 0;
        char *end = NULL;
        while (!end && hl < sizeof(head) - 1) {
            ssize_t n = read(s, head + hl, sizeof(head) - 1 - hl);
            if (n <= 0)
                break;
            hl += (size_t)n;
            head[hl] = 0;
            end = strstr(head, "\r\n\r\n");
        }
        if (!end) {
            fprintf(stderr, "wget: resposta sem cabecalho completo\n");
            close(s);
            return 1;
        }
        int code = 0;
        sscanf(head, "HTTP/%*s %d", &code);
        size_t body_off = (size_t)(end - head) + 4;
        *end = 0;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            const char *loc = header_value(head, "Location");
            close(s);
            if (!loc) {
                fprintf(stderr, "wget: redirecionamento %d sem Location\n", code);
                return 1;
            }
            char next[1300];
            size_t ll = strcspn(loc, "\r\n");
            if (loc[0] == '/')
                snprintf(next, sizeof(next), "http://%s:%d%.*s", u.host, u.port, (int)ll, loc);
            else
                snprintf(next, sizeof(next), "%.*s", (int)ll, loc);
            if (!quiet)
                fprintf(stderr, "wget: %d, seguindo para %s\n", code, next);
            if (parse_url(next, &u) < 0)
                return 1;
            continue;
        }
        if (code != 200) {
            fprintf(stderr, "wget: o servidor respondeu %d\n", code);
            close(s);
            return 1;
        }
        const char *name = out_name ? out_name : default_name(u.path);
        int out = strcmp(name, "-") == 0 ? STDOUT_FILENO : open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "wget: %s: %s\n", name, strerror(errno));
            close(s);
            return 1;
        }
        long long total = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (hl > body_off) {
            write_all(out, head + body_off, hl - body_off);
            total += (long long)(hl - body_off);
        }
        char buf[16384];
        ssize_t n;
        while ((n = read(s, buf, sizeof(buf))) > 0) {
            if (write_all(out, buf, (size_t)n) < 0) {
                fprintf(stderr, "wget: gravando %s: %s\n", name, strerror(errno));
                close(s);
                return 1;
            }
            total += n;
        }
        close(s);
        if (out != STDOUT_FILENO)
            close(out);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (n < 0) {
            fprintf(stderr, "wget: leitura: %s\n", strerror(errno));
            return 1;
        }
        if (!quiet)
            fprintf(stderr, "wget: %s salvo (%lld bytes em %.2f s)\n", name, total, dt);
        return 0;
    }
    fprintf(stderr, "wget: redirecionamentos demais\n");
    return 1;
}
