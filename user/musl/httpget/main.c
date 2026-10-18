#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t c, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static int resolve(const char *host, struct in_addr *out)
{
    if (inet_pton(AF_INET, host, out) == 1)
        return 0;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, NULL, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "httpget: %s: %s\n", host, gai_strerror(r));
        return -1;
    }
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *outfile = NULL;
    int quiet = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outfile = argv[++i];
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else {
            fprintf(stderr, "uso: httpget [-q] [-o arquivo] <host> [porta] [caminho]\n");
            return 2;
        }
    }
    if (i >= argc) {
        fprintf(stderr, "uso: httpget [-q] [-o arquivo] <host> [porta] [caminho]\n");
        return 2;
    }
    const char *host = argv[i];
    int port = i + 1 < argc ? atoi(argv[i + 1]) : 80;
    const char *path = i + 2 < argc ? argv[i + 2] : "/";
    setvbuf(stdout, NULL, _IOLBF, 0);
    crc_init();

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (resolve(host, &sa.sin_addr) < 0)
        return 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "httpget: socket: %s\n", strerror(errno));
        return 1;
    }
    double t0 = now_s();
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "httpget: connect %s:%d: %s\n", inet_ntoa(sa.sin_addr), port, strerror(errno));
        close(s);
        return 1;
    }
    char req[1024];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: orion-httpget\r\nConnection: close\r\n\r\n",
                      path, host);
    if (write_all(s, req, (size_t)rl) < 0) {
        fprintf(stderr, "httpget: envio: %s\n", strerror(errno));
        close(s);
        return 1;
    }
    int out = -1;
    if (outfile) {
        out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            fprintf(stderr, "httpget: %s: %s\n", outfile, strerror(errno));
            close(s);
            return 1;
        }
    }
    char head[4096];
    size_t hl = 0;
    int in_body = 0;
    unsigned long long body = 0;
    uint32_t crc = 0xFFFFFFFFu;
    char buf[8192];
    char status[128] = "";
    for (;;) {
        ssize_t n = read(s, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "httpget: leitura: %s\n", strerror(errno));
            close(s);
            return 1;
        }
        if (n == 0)
            break;
        const char *p = buf;
        size_t left = (size_t)n;
        if (!in_body) {
            size_t take = left < sizeof(head) - 1 - hl ? left : sizeof(head) - 1 - hl;
            memcpy(head + hl, p, take);
            hl += take;
            head[hl] = 0;
            char *end = strstr(head, "\r\n\r\n");
            if (!end && hl == sizeof(head) - 1) {
                fprintf(stderr, "httpget: cabecalho grande demais\n");
                close(s);
                return 1;
            }
            if (!end)
                continue;
            size_t hdr = (size_t)(end - head) + 4;
            size_t first = strcspn(head, "\r\n");
            snprintf(status, sizeof(status), "%.*s", (int)first, head);
            size_t consumed_before = hl - take;
            size_t skip = hdr - consumed_before;
            p += skip;
            left -= skip;
            in_body = 1;
        }
        if (left) {
            crc = crc_update(crc, (const unsigned char *)p, left);
            body += left;
            if (out >= 0) {
                if (write_all(out, p, left) < 0) {
                    fprintf(stderr, "httpget: gravando %s: %s\n", outfile, strerror(errno));
                    close(s);
                    return 1;
                }
            } else if (!quiet) {
                fwrite(p, 1, left, stdout);
            }
        }
    }
    double dt = now_s() - t0;
    close(s);
    if (out >= 0)
        close(out);
    if (!in_body) {
        fprintf(stderr, "httpget: resposta sem cabecalho completo\n");
        return 1;
    }
    if (!quiet && out < 0)
        printf("\n");
    printf("httpget: %s\n", status);
    printf("httpget: corpo de %llu bytes, crc32 %08x, %.2f s\n", body, crc ^ 0xFFFFFFFFu, dt);
    return strstr(status, " 200") ? 0 : 1;
}
