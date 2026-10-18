#pragma once

#include <kernel/compiler.h>

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_TYPE_MASK 0xF
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

#define SIOCGIFCONF_ORION 0x8900
#define SIOCSIFCONF_ORION 0x8901
#define SIOCDHCP_ORION    0x8902

struct ifreq_orion {
    uint8_t  mac[6];
    uint8_t  link_up;
    uint8_t  pad;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t irqs;
};

long sys_socket(int domain, int type, int protocol);
long sys_bind(int fd, const struct sockaddr_in *addr, uint32_t len);
long sys_connect(int fd, const struct sockaddr_in *addr, uint32_t len);
long sys_sendto(int fd, const void *buf, uint64_t len, int flags, const struct sockaddr_in *addr,
                uint32_t addrlen);
long sys_recvfrom(int fd, void *buf, uint64_t len, int flags, struct sockaddr_in *addr,
                  uint32_t *addrlen);

#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SOL_TCP       6
#define TCP_NODELAY   1
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

struct user_msghdr;
long sys_listen(int fd, int backlog);
long sys_socketpair(int domain, int type, int protocol, int *usv);
long sys_accept4(int fd, struct sockaddr_in *addr, uint32_t *len, int flags);
long sys_shutdown(int fd, int how);
long sys_getsockname(int fd, struct sockaddr_in *addr, uint32_t *len);
long sys_getpeername(int fd, struct sockaddr_in *addr, uint32_t *len);
long sys_setsockopt(int fd, int level, int name, const void *val, uint32_t len);
long sys_getsockopt(int fd, int level, int name, void *val, uint32_t *len);
long sys_sendmsg(int fd, const struct user_msghdr *msg, int flags);
long sys_recvmsg(int fd, struct user_msghdr *msg, int flags);
