#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <orion.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DHCP_MAGIC   0x63825363u
#define OPT_PAD      0
#define OPT_MASK     1
#define OPT_ROUTER   3
#define OPT_DNS      6
#define OPT_REQ_IP   50
#define OPT_LEASE    51
#define OPT_TYPE     53
#define OPT_SERVER   54
#define OPT_PARAMS   55
#define OPT_END      255
#define DISCOVER     1
#define OFFER        2
#define REQUEST      3
#define ACK          5
#define NAK          6

struct bootp {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

struct lease {
    uint32_t ip, mask, router, dns, server, seconds;
    int type;
};

static int quiet;

static uint32_t random32(void)
{
    uint32_t v = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != sizeof(v))
            v ^= 0x9E3779B9u;
        close(fd);
    }
    return v;
}

static size_t build(struct bootp *m, const uint8_t mac[6], uint32_t xid, int type, uint32_t want, uint32_t server)
{
    memset(m, 0, sizeof(*m));
    m->op = 1;
    m->htype = 1;
    m->hlen = 6;
    m->xid = xid;
    m->flags = htons(0x8000);
    memcpy(m->chaddr, mac, 6);
    m->magic = htonl(DHCP_MAGIC);
    uint8_t *o = m->options;
    *o++ = OPT_TYPE;
    *o++ = 1;
    *o++ = (uint8_t)type;
    if (want) {
        *o++ = OPT_REQ_IP;
        *o++ = 4;
        memcpy(o, &want, 4);
        o += 4;
    }
    if (server) {
        *o++ = OPT_SERVER;
        *o++ = 4;
        memcpy(o, &server, 4);
        o += 4;
    }
    *o++ = OPT_PARAMS;
    *o++ = 4;
    *o++ = OPT_MASK;
    *o++ = OPT_ROUTER;
    *o++ = OPT_DNS;
    *o++ = OPT_LEASE;
    *o++ = OPT_END;
    return (size_t)(o - (uint8_t *)m);
}

static int parse(const struct bootp *m, size_t len, uint32_t xid, struct lease *l)
{
    if (len < offsetof(struct bootp, options) || m->op != 2 || m->xid != xid || ntohl(m->magic) != DHCP_MAGIC)
        return -1;
    memset(l, 0, sizeof(*l));
    l->ip = m->yiaddr;
    const uint8_t *o = m->options, *end = (const uint8_t *)m + len;
    while (o < end && *o != OPT_END) {
        if (*o == OPT_PAD) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        uint8_t code = o[0], n = o[1];
        const uint8_t *v = o + 2;
        if (code == OPT_TYPE && n >= 1)
            l->type = v[0];
        else if (code == OPT_MASK && n >= 4)
            memcpy(&l->mask, v, 4);
        else if (code == OPT_ROUTER && n >= 4)
            memcpy(&l->router, v, 4);
        else if (code == OPT_DNS && n >= 4)
            memcpy(&l->dns, v, 4);
        else if (code == OPT_SERVER && n >= 4)
            memcpy(&l->server, v, 4);
        else if (code == OPT_LEASE && n >= 4) {
            uint32_t t;
            memcpy(&t, v, 4);
            l->seconds = ntohl(t);
        }
        o += 2 + n;
    }
    return l->type ? 0 : -1;
}

static int exchange(int s, const struct bootp *out, size_t len, uint32_t xid, int want, struct lease *l)
{
    struct sockaddr_in to = { 0 };
    to.sin_family = AF_INET;
    to.sin_port = htons(67);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    for (int attempt = 0; attempt < 4; attempt++) {
        if (sendto(s, out, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            fprintf(stderr, "dhcp: envio: %s\n", strerror(errno));
            return -1;
        }
        int wait_ms = 1000 << attempt;
        struct timespec t0, now;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            int left = wait_ms - (int)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (left <= 0)
                break;
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, left) <= 0)
                continue;
            struct bootp in;
            ssize_t n = recv(s, &in, sizeof(in), 0);
            if (n <= 0)
                continue;
            if (parse(&in, (size_t)n, xid, l) == 0 && (l->type == want || l->type == NAK))
                return 0;
        }
    }
    return -1;
}

static void dotted(uint32_t be, char *out)
{
    struct in_addr a = { be };
    snprintf(out, 16, "%s", inet_ntoa(a));
}

int main(int argc, char **argv)
{
    int keep_resolv = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else if (strcmp(argv[i], "-n") == 0)
            keep_resolv = 1;
        else {
            fprintf(stderr, "uso: dhcp [-q] [-n]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        fprintf(stderr, "dhcp: sem rede: %s\n", strerror(errno));
        return 1;
    }
    struct ifreq_orion ifr;
    memset(&ifr, 0, sizeof(ifr));
    if (ioctl(s, SIOCGIFCONF_ORION, &ifr) < 0) {
        fprintf(stderr, "dhcp: interface: %s\n", strerror(errno));
        return 1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in me = { 0 };
    me.sin_family = AF_INET;
    me.sin_port = htons(68);
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        fprintf(stderr, "dhcp: porta 68: %s\n", strerror(errno));
        return 1;
    }
    if (!quiet)
        printf("dhcp: procurando servidor DHCP...\n");
    struct bootp m;
    struct lease offer, ack;
    for (int round = 0; round < 3; round++) {
        uint32_t xid = random32();
        size_t len = build(&m, ifr.mac, xid, DISCOVER, 0, 0);
        if (exchange(s, &m, len, xid, OFFER, &offer) < 0 || offer.type != OFFER)
            continue;
        len = build(&m, ifr.mac, xid, REQUEST, offer.ip, offer.server);
        if (exchange(s, &m, len, xid, ACK, &ack) < 0)
            continue;
        if (ack.type == NAK) {
            if (!quiet)
                printf("dhcp: o servidor recusou (NAK), tentando de novo\n");
            continue;
        }
        struct ifreq_orion set = ifr;
        set.ip = ntohl(ack.ip);
        set.netmask = ntohl(ack.mask ? ack.mask : offer.mask);
        set.gateway = ntohl(ack.router ? ack.router : offer.router);
        set.dns = ntohl(ack.dns ? ack.dns : offer.dns);
        if (ioctl(s, SIOCSIFCONF_ORION, &set) < 0) {
            fprintf(stderr, "dhcp: configurando a interface: %s\n", strerror(errno));
            return 1;
        }
        char ip[16], mask[16], gw[16], dns[16], srv[16];
        dotted(htonl(set.ip), ip);
        dotted(htonl(set.netmask), mask);
        dotted(htonl(set.gateway), gw);
        dotted(htonl(set.dns), dns);
        dotted(ack.server ? ack.server : offer.server, srv);
        if (set.dns && !keep_resolv) {
            FILE *f = fopen("/etc/resolv.conf", "w");
            if (f) {
                fprintf(f, "nameserver %s\n", dns);
                fclose(f);
            }
        }
        printf("dhcp: ip %s mascara %s gateway %s dns %s (servidor %s, concessao de %u s)\n", ip, mask, gw, dns,
               srv, ack.seconds);
        close(s);
        return 0;
    }
    fprintf(stderr, "dhcp: nenhum servidor respondeu\n");
    close(s);
    return 1;
}
