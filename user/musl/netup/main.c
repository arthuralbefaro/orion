#include <arpa/inet.h>
#include <errno.h>
#include <orion.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONFIG "/etc/network"

static int run_dhcp(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *args[] = { "dhcp", NULL };
        execv("/bin/dhcp", args);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int parse_ip(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1)
        return -1;
    *out = ntohl(a.s_addr);
    return 0;
}

static int set_static(char **f, int n)
{
    if (n < 5) {
        fprintf(stderr, "netup: static precisa de ip, mascara, gateway e dns\n");
        return -1;
    }
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq_orion r;
    memset(&r, 0, sizeof(r));
    if (s < 0 || ioctl(s, SIOCGIFCONF_ORION, &r) < 0) {
        fprintf(stderr, "netup: interface: %s\n", strerror(errno));
        return -1;
    }
    if (parse_ip(f[2], &r.ip) < 0 || parse_ip(f[3], &r.netmask) < 0 || parse_ip(f[4], &r.gateway) < 0 ||
        (n > 5 && parse_ip(f[5], &r.dns) < 0)) {
        fprintf(stderr, "netup: endereco invalido em %s\n", CONFIG);
        close(s);
        return -1;
    }
    int rc = ioctl(s, SIOCSIFCONF_ORION, &r);
    close(s);
    if (rc < 0) {
        fprintf(stderr, "netup: configurando: %s\n", strerror(errno));
        return -1;
    }
    if (n > 5) {
        FILE *rf = fopen("/etc/resolv.conf", "w");
        if (rf) {
            fprintf(rf, "nameserver %s\n", f[5]);
            fclose(rf);
        }
    }
    printf("netup: %s estatico: ip %s mascara %s gateway %s%s%s\n", f[0], f[2], f[3], f[4], n > 5 ? " dns " : "",
           n > 5 ? f[5] : "");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    FILE *cf = fopen(CONFIG, "r");
    if (!cf) {
        fprintf(stderr, "netup: %s: %s\n", CONFIG, strerror(errno));
        return 1;
    }
    char line[256];
    int done = 0, bad = 0;
    while (fgets(line, sizeof(line), cf)) {
        char *f[8];
        int n = 0;
        for (char *t = strtok(line, " \t\r\n"); t && n < 8; t = strtok(NULL, " \t\r\n"))
            f[n++] = t;
        if (n == 0 || f[0][0] == '#')
            continue;
        if (strcmp(f[0], "eth0") != 0) {
            fprintf(stderr, "netup: interface desconhecida: %s\n", f[0]);
            bad = 1;
            continue;
        }
        if (n >= 2 && strcmp(f[1], "dhcp") == 0) {
            printf("netup: eth0 por DHCP\n");
            if (run_dhcp() < 0)
                bad = 1;
            done = 1;
        } else if (n >= 2 && strcmp(f[1], "static") == 0) {
            if (set_static(f, n) < 0)
                bad = 1;
            done = 1;
        } else if (n >= 2 && strcmp(f[1], "off") == 0) {
            printf("netup: eth0 desligada na configuracao\n");
            done = 1;
        } else {
            fprintf(stderr, "netup: linha invalida para eth0 em %s\n", CONFIG);
            bad = 1;
        }
    }
    fclose(cf);
    if (!done && !bad)
        printf("netup: nada a configurar em %s\n", CONFIG);
    return bad;
}
