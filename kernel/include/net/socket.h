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

struct user_msghdr;
long sys_sendmsg(int fd, const struct user_msghdr *msg, int flags);
long sys_recvmsg(int fd, struct user_msghdr *msg, int flags);
