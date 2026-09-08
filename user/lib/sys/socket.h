#pragma once

#include <sys/types.h>

#define AF_INET     2
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17

#define INADDR_ANY       0u
#define INADDR_BROADCAST 0xFFFFFFFFu

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

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr_in *addr, uint32_t len);
int connect(int fd, const struct sockaddr_in *addr, uint32_t len);
long sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr_in *addr,
            uint32_t addrlen);
long recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr_in *addr,
              uint32_t *addrlen);

uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

/* Endereco em ordem de host: 10.0.2.15 vira 0x0A00020F. */
int   inet_aton(const char *s, uint32_t *out);
char *inet_ntoa(uint32_t ip, char *buf, size_t size);
