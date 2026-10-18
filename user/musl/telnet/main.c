#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

enum { DATA, GOT_IAC, GOT_OPT, IN_SB, SB_IAC };

static int sock;
static int state = DATA;
static int verb;

static int write_all(int fd, const unsigned char *p, size_t n)
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

static void answer(int v, int opt)
{
    unsigned char r[3] = { IAC, 0, (unsigned char)opt };
    if (v == WILL)
        r[1] = DONT;
    else if (v == DO)
        r[1] = WONT;
    else
        return;
    write_all(sock, r, 3);
}

static void from_net(const unsigned char *in, size_t n)
{
    unsigned char out[8192];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = in[i];
        switch (state) {
        case DATA:
            if (c == IAC)
                state = GOT_IAC;
            else
                out[o++] = c;
            break;
        case GOT_IAC:
            if (c == IAC) {
                out[o++] = IAC;
                state = DATA;
            } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                verb = c;
                state = GOT_OPT;
            } else if (c == SB) {
                state = IN_SB;
            } else {
                state = DATA;
            }
            break;
        case GOT_OPT:
            answer(verb, c);
            state = DATA;
            break;
        case IN_SB:
            if (c == IAC)
                state = SB_IAC;
            break;
        case SB_IAC:
            state = c == SE ? DATA : IN_SB;
            break;
        }
    }
    write_all(STDOUT_FILENO, out, o);
}

static int to_net(const unsigned char *in, size_t n)
{
    unsigned char out[16384 * 2];
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == IAC)
            out[o++] = IAC;
        if (in[i] == '\n')
            out[o++] = '\r';
        out[o++] = in[i];
    }
    return write_all(sock, out, o);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "uso: telnet host [porta]\n");
        return 2;
    }
    int port = argc == 3 ? atoi(argv[2]) : 23;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, argv[1], &sa.sin_addr) != 1) {
        struct addrinfo hints = { 0 }, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int r = getaddrinfo(argv[1], NULL, &hints, &res);
        if (r != 0) {
            fprintf(stderr, "telnet: %s: %s\n", argv[1], gai_strerror(r));
            return 1;
        }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    signal(SIGPIPE, SIG_IGN);
    sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0 || connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "telnet: %s %d: %s\n", argv[1], port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "telnet: conectado a %s %d\n", argv[1], port);
    int in_open = 1;
    unsigned char buf[16384];
    for (;;) {
        struct pollfd fds[2] = { { sock, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        if (poll(fds, in_open ? 2 : 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n <= 0)
                break;
            from_net(buf, (size_t)n);
        }
        if (in_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                in_open = 0;
                shutdown(sock, SHUT_WR);
            } else if (to_net(buf, (size_t)n) < 0) {
                break;
            }
        }
    }
    close(sock);
    fprintf(stderr, "telnet: conexao encerrada\n");
    return 0;
}
