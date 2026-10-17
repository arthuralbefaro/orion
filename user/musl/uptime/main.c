#include <orion.h>
#include <stdio.h>

int main(void)
{
    struct sysinfo_orion si;
    if (sysinfo_orion(&si) < 0) {
        fprintf(stderr, "uptime: sysinfo falhou\n");
        return 1;
    }
    uint64_t s = si.uptime_ms / 1000;
    uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    printf("ligado ha ");
    if (d)
        printf("%lu dia(s), ", (unsigned long)d);
    printf("%02lu:%02lu:%02lu, %u processo(s), %u CPU(s)\n", (unsigned long)h, (unsigned long)m,
           (unsigned long)(s % 60), si.procs, si.cpus);
    return 0;
}