#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>

static int unit_shift = 10;
static const char *unit_name = "KiB";

static unsigned long pages(uint64_t n)
{
    return (unsigned long)((n * 4096) >> unit_shift);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-m") == 0) {
        unit_shift = 20;
        unit_name = "MiB";
    }

    struct sysinfo_orion si;
    if (sysinfo_orion(&si) < 0) {
        fprintf(stderr, "free: sysinfo falhou\n");
        return 1;
    }

    printf("em %s     %10s %10s %10s %14s\n", unit_name, "total", "usada", "livre", "compartilhada");
    printf("memoria:       %10lu %10lu %10lu %14lu\n", pages(si.total_pages), pages(si.used_pages),
           pages(si.free_pages), pages(si.shared_pages));
    printf("heap do kernel:            %10lu\n", (unsigned long)(si.heap_used >> unit_shift));
    return 0;
}
