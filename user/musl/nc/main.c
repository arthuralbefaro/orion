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

static int resolve(const char *host, int port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1)
        return 0;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, NULL, &hints, &res);
    if (r != 0) {
        fprintf(stderr, "nc: %s: %s\n", host, gai_strerror(r));
        return -1;
    }
    sa->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

static int relay(int s, int idle_ms)
{
    int in_open = 1;
    char buf[8192];
    for (;;) {
        struct pollfd fds[2] = { { s, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        int r = poll(fds, in_open ? 2 : 1, idle_ms > 0 ? idle_ms : -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (r == 0)
            return 0;
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(s, buf, sizeof(buf));
            if (n <= 0)
                return n < 0;
            if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0)
                return 1;
        }
        if (in_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                in_open = 0;
                shutdown(s, SHUT_WR);
            } else if (write_all(s, buf, (size_t)n) < 0) {
                return 1;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int listen_mode = 0, scan = 0, port = 0, wait_s = 0;
    const char *host = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-l") == 0)
            listen_mode = 1;
        else if (strcmp(argv[i], "-z") == 0)
            scan = 1;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            wait_s = atoi(argv[++i]);
        else
            break;
    }
    if (i < argc && !listen_mode)
        host = argv[i++];
    if (i < argc)
        port = atoi(argv[i++]);
    if (port <= 0 || port > 65535 || (!listen_mode && !host) || i != argc) {
        fprintf(stderr, "uso: nc [-z] [-w segundos] host porta | nc -l [-w segundos] [-p] porta\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    int s;
    if (listen_mode) {
        int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a = { 0 };
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(l, 1) < 0) {
            fprintf(stderr, "nc: porta %d: %s\n", port, strerror(errno));
            return 1;
        }
        s = accept4(l, NULL, NULL, SOCK_CLOEXEC);
        close(l);
        if (s < 0) {
            fprintf(stderr, "nc: accept: %s\n", strerror(errno));
            return 1;
        }
    } else {
        struct sockaddr_in sa;
        if (resolve(host, port, &sa) < 0)
            return 1;
        s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            if (scan) {
                printf("nc: %s %d fechada (%s)\n", host, port, strerror(errno));
                return 1;
            }
            fprintf(stderr, "nc: %s %d: %s\n", host, port, strerror(errno));
            return 1;
        }
        if (scan) {
            printf("nc: %s %d aberta\n", host, port);
            close(s);
            return 0;
        }
    }
    int r = relay(s, wait_s * 1000);
    close(s);
    return r;
}
