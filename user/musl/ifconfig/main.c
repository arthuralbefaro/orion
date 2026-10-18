#include <errno.h>
#include <orion.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void show(const struct ifreq_orion *r)
{
    char ip[16], mask[16], gw[16], dns[16];
    printf("eth0  %02x:%02x:%02x:%02x:%02x:%02x  link %s\n", r->mac[0], r->mac[1], r->mac[2],
           r->mac[3], r->mac[4], r->mac[5], r->link_up ? "up" : "down");
    printf("      ip %s  mascara %s\n", orion_inet_ntoa(r->ip, ip, sizeof(ip)),
           orion_inet_ntoa(r->netmask, mask, sizeof(mask)));
    printf("      gateway %s  dns %s\n", orion_inet_ntoa(r->gateway, gw, sizeof(gw)),
           orion_inet_ntoa(r->dns, dns, sizeof(dns)));
    printf("      rx %lu  tx %lu  descartados %lu  interrupcoes %lu\n", r->rx_packets,
           r->tx_packets, r->rx_dropped, r->irqs);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "ifconfig: sem rede: %s\n", orion_strerror(errno));
        return 1;
    }

    struct ifreq_orion r;
    memset(&r, 0, sizeof(r));

    if (argc == 1) {
        if (ioctl(fd, SIOCGIFCONF_ORION, &r) < 0) {
            fprintf(stderr, "ifconfig: %s\n", orion_strerror(errno));
            return 1;
        }
        show(&r);
        close(fd);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "dhcp") == 0) {
        close(fd);
        char *args[] = { "dhcp", NULL };
        execv("/bin/dhcp", args);
        fprintf(stderr, "ifconfig: /bin/dhcp: %s\n", orion_strerror(errno));
        return 1;
    }

    if (argc != 5) {
        fprintf(stderr, "uso: ifconfig [dhcp | <ip> <mascara> <gateway> <dns>]\n");
        return 1;
    }
    if (!orion_inet_aton(argv[1], &r.ip) || !orion_inet_aton(argv[2], &r.netmask) ||
        !orion_inet_aton(argv[3], &r.gateway) || !orion_inet_aton(argv[4], &r.dns)) {
        fprintf(stderr, "ifconfig: endereco invalido\n");
        return 1;
    }
    if (ioctl(fd, SIOCSIFCONF_ORION, &r) < 0) {
        fprintf(stderr, "ifconfig: %s\n", orion_strerror(errno));
        return 1;
    }
    ioctl(fd, SIOCGIFCONF_ORION, &r);
    show(&r);
    close(fd);
    return 0;
}
