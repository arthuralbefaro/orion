#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "dhcp: sem rede: %s\n", strerror(errno));
        return 1;
    }
    printf("procurando servidor DHCP...\n");
    if (ioctl(fd, SIOCDHCP_ORION, 0) < 0) {
        fprintf(stderr, "dhcp: falhou: %s\n", strerror(errno));
        return 1;
    }
    struct ifreq_orion r;
    memset(&r, 0, sizeof(r));
    ioctl(fd, SIOCGIFCONF_ORION, &r);
    char ip[16], mask[16], gw[16], dns[16];
    printf("ip %s  mascara %s  gateway %s  dns %s\n", inet_ntoa(r.ip, ip, sizeof(ip)),
           inet_ntoa(r.netmask, mask, sizeof(mask)), inet_ntoa(r.gateway, gw, sizeof(gw)),
           inet_ntoa(r.dns, dns, sizeof(dns)));
    close(fd);
    return 0;
}
