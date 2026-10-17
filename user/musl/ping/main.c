#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <orion.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct icmp_echo {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
    uint8_t  data[56];
};

static volatile int stop;
static int sent, received;

static void on_int(int sig)
{
    (void)sig;
    stop = 1;
}

static uint16_t checksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: ping <ip> [quantidade]\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t dst;
    if (!orion_inet_aton(argv[1], &dst)) {
        fprintf(stderr, "ping: '%s' nao e um endereco IP\n", argv[1]);
        return 1;
    }
    int count = argc > 2 ? atoi(argv[2]) : 4;
    if (count <= 0)
        count = 4;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        fprintf(stderr, "ping: %s\n", orion_strerror(errno));
        return 1;
    }
    signal(SIGINT, on_int);

    char text[16];
    printf("PING %s: 56 bytes\n", orion_inet_ntoa(dst, text, sizeof(text)));

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(dst);

    uint16_t id = (uint16_t)getpid();
    long total_us = 0, min_us = 0, max_us = 0;

    for (int i = 0; i < count && !stop; i++) {
        struct icmp_echo pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = 8;
        pkt.id = htons(id);
        pkt.seq = htons((uint16_t)(i + 1));
        for (int k = 0; k < (int)sizeof(pkt.data); k++)
            pkt.data[k] = (uint8_t)('a' + k % 26);
        pkt.checksum = htons(checksum(&pkt, (int)sizeof(pkt)));

        long start = now_us();
        if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            fprintf(stderr, "ping: %s\n", orion_strerror(errno));
            break;
        }
        sent++;

        alarm(2);
        struct icmp_echo reply;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        long n = recvfrom(fd, &reply, sizeof(reply), 0, (struct sockaddr *)&from, &fromlen);
        alarm(0);

        if (n < 0) {
            printf("  seq %d: sem resposta\n", i + 1);
        } else {
            long us = now_us() - start;
            received++;
            total_us += us;
            if (!min_us || us < min_us)
                min_us = us;
            if (us > max_us)
                max_us = us;
            printf("  %ld bytes de %s: seq=%u tempo=%ld.%03ld ms\n", n,
                   orion_inet_ntoa(ntohl(from.sin_addr.s_addr), text, sizeof(text)), ntohs(reply.seq),
                   us / 1000, us % 1000);
        }
        if (i + 1 < count && !stop)
            sleep(1);
    }

    close(fd);
    int loss = sent ? (sent - received) * 100 / sent : 0;
    printf("--- %s ---\n", argv[1]);
    printf("%d enviado(s), %d recebido(s), %d%% de perda\n", sent, received, loss);
    if (received)
        printf("min %ld.%03ld ms  medio %ld.%03ld ms  max %ld.%03ld ms\n", min_us / 1000,
               min_us % 1000, total_us / received / 1000, (total_us / received) % 1000,
               max_us / 1000, max_us % 1000);
    return received ? 0 : 1;
}
