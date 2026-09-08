#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>

#include "syscall.h"

static long ret_errno(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int socket(int domain, int type, int protocol)
{
    return (int)ret_errno(syscall3(SYS_SOCKET, domain, type, protocol));
}

int bind(int fd, const struct sockaddr_in *addr, uint32_t len)
{
    return (int)ret_errno(syscall3(SYS_BIND, fd, (long)addr, len));
}

int connect(int fd, const struct sockaddr_in *addr, uint32_t len)
{
    return (int)ret_errno(syscall3(SYS_CONNECT, fd, (long)addr, len));
}

long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr_in *addr,
            uint32_t addrlen)
{
    return ret_errno(syscall6(SYS_SENDTO, fd, (long)buf, (long)len, flags, (long)addr, addrlen));
}

long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr_in *addr,
              uint32_t *addrlen)
{
    return ret_errno(syscall6(SYS_RECVFROM, fd, (long)buf, (long)len, flags, (long)addr,
                              (long)addrlen));
}

uint16_t htons(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

uint32_t htonl(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}

int inet_aton(const char *s, uint32_t *out)
{
    uint32_t parts[4] = { 0 };
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9')
            return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return 0;
            s++;
        }
        parts[n++] = v;
        if (n < 4) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    if (*s)
        return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

char *inet_ntoa(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
             ip & 0xFF);
    return buf;
}
