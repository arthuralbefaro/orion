#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#define DNS_PORT 53

static int encode_name(const char *host, uint8_t *out, int max)
{
    int at = 0;
    const char *label = host;
    for (;;) {
        const char *dot = strchr(label, '.');
        int len = dot ? (int)(dot - label) : (int)strlen(label);
        if (len == 0 || len > 63 || at + len + 1 >= max)
            return -1;
        out[at++] = (uint8_t)len;
        memcpy(out + at, label, (size_t)len);
        at += len;
        if (!dot)
            break;
        label = dot + 1;
    }
    if (at >= max)
        return -1;
    out[at++] = 0;
    return at;
}

static int skip_name(const uint8_t *msg, int len, int at)
{
    while (at < len) {
        uint8_t n = msg[at];
        if (n == 0)
            return at + 1;
        if ((n & 0xC0) == 0xC0)
            return at + 2;
        at += n + 1;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: nslookup <nome> [servidor]\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "nslookup: %s\n", strerror(errno));
        return 1;
    }

    uint32_t server = 0;
    if (argc > 2) {
        if (!inet_aton(argv[2], &server)) {
            fprintf(stderr, "nslookup: servidor invalido\n");
            return 1;
        }
    } else {
        struct ifreq_orion r;
        memset(&r, 0, sizeof(r));
        if (ioctl(fd, SIOCGIFCONF_ORION, &r) < 0 || !r.dns) {
            fprintf(stderr, "nslookup: nenhum servidor DNS configurado (rode 'ifconfig dhcp')\n");
            return 1;
        }
        server = r.dns;
    }

    uint8_t query[512];
    memset(query, 0, sizeof(query));
    query[0] = 0x4F;
    query[1] = 0x52;
    query[2] = 0x01;
    query[5] = 0x01;
    int at = 12;
    int n = encode_name(argv[1], query + at, (int)sizeof(query) - at);
    if (n < 0) {
        fprintf(stderr, "nslookup: nome invalido\n");
        return 1;
    }
    at += n;
    query[at++] = 0;
    query[at++] = 1;
    query[at++] = 0;
    query[at++] = 1;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr = htonl(server);
    to.sin_port = htons(DNS_PORT);

    char text[16];
    printf("servidor: %s\n", inet_ntoa(server, text, sizeof(text)));

    if (sendto(fd, query, (size_t)at, 0, &to, sizeof(to)) < 0) {
        fprintf(stderr, "nslookup: %s\n", strerror(errno));
        return 1;
    }

    alarm(5);
    uint8_t reply[512];
    long got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
    alarm(0);
    if (got < 12) {
        fprintf(stderr, "nslookup: sem resposta\n");
        return 1;
    }

    int qd = (reply[4] << 8) | reply[5];
    int an = (reply[6] << 8) | reply[7];
    if ((reply[3] & 0x0F) != 0) {
        printf("%s: nao encontrado (codigo %d)\n", argv[1], reply[3] & 0x0F);
        return 1;
    }

    at = 12;
    for (int i = 0; i < qd; i++) {
        at = skip_name(reply, (int)got, at);
        if (at < 0)
            return 1;
        at += 4;
    }

    int found = 0;
    for (int i = 0; i < an && at + 10 <= got; i++) {
        at = skip_name(reply, (int)got, at);
        if (at < 0 || at + 10 > got)
            break;
        int type = (reply[at] << 8) | reply[at + 1];
        int rdlen = (reply[at + 8] << 8) | reply[at + 9];
        at += 10;
        if (type == 1 && rdlen == 4 && at + 4 <= got) {
            uint32_t ip = ((uint32_t)reply[at] << 24) | ((uint32_t)reply[at + 1] << 16) |
                          ((uint32_t)reply[at + 2] << 8) | reply[at + 3];
            printf("%s tem endereco %s\n", argv[1], inet_ntoa(ip, text, sizeof(text)));
            found++;
        }
        at += rdlen;
    }

    close(fd);
    if (!found) {
        printf("%s: nenhum registro A na resposta\n", argv[1]);
        return 1;
    }
    return 0;
}
