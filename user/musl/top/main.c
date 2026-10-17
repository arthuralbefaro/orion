#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <orion.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static volatile int stop;

static void on_int(int sig)
{
    (void)sig;
    stop = 1;
}

static const char *state_name(uint32_t s)
{
    return s == 0 ? "rodando" : "zumbi";
}

static void draw(void)
{
    struct sysinfo_orion si;
    if (orion_sysinfo(&si) < 0) {
        fprintf(stderr, "top: sysinfo falhou\n");
        stop = 1;
        return;
    }
    struct procinfo_orion list[PROCLIST_MAX];
    int n = orion_proclist(list, PROCLIST_MAX);
    if (n < 0)
        n = 0;

    fputs("\x1b[H\x1b[2J", stdout);
    printf("\x1b[7m Orion top   %lu processo(s)   %u CPU(s)   uptime %lu.%03lu s %*s\x1b[0m\n",
           (unsigned long)n, si.cpus, si.uptime_ms / 1000, si.uptime_ms % 1000, 18, "");
    printf("memoria: %lu de %lu paginas usadas (%lu MiB de %lu MiB), heap %lu KiB\n",
           si.used_pages, si.total_pages, si.used_pages * 4096 / (1024 * 1024),
           si.total_pages * 4096 / (1024 * 1024), si.heap_used / 1024);
    printf("copy-on-write: %lu pagina(s) compartilhada(s), %lu falta(s)\n\n",
           si.shared_pages, si.cow_faults);
    printf("%6s %6s %6s %10s %10s  %s\n", "pid", "ppid", "pgid", "paginas", "estado", "nome");
    for (int i = 0; i < n; i++)
        printf("%6u %6u %6u %10lu %10s  %s\n", list[i].pid, list[i].ppid, list[i].pgid,
               list[i].pages, state_name(list[i].state), list[i].name);
    printf("\nCtrl+C sai.\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 0;
    signal(SIGINT, on_int);

    for (int i = 0; !stop && (rounds == 0 || i < rounds); i++) {
        draw();
        if (stop)
            break;
        if (rounds == 0 || i + 1 < rounds)
            sleep(1);
    }
    if (rounds == 0)
        fputs("\n", stdout);
    return 0;
}
