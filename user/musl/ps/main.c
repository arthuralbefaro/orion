#include <orion.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int full = argc > 1 && strcmp(argv[1], "-l") == 0;
    struct procinfo_orion list[PROCLIST_MAX];
    int n = proclist_orion(list, PROCLIST_MAX);
    if (n < 0) {
        fprintf(stderr, "ps: proclist falhou\n");
        return 1;
    }

    if (full)
        printf("%5s %5s %5s %8s %-7s %s\n", "PID", "PPID", "PGID", "PAGINAS", "ESTADO", "NOME");
    else
        printf("%5s %-7s %s\n", "PID", "ESTADO", "NOME");

    for (int i = n - 1; i >= 0; i--) {
        const char *state = list[i].state == 0 ? "rodando" : "zumbi";
        if (full)
            printf("%5u %5u %5u %8lu %-7s %s\n", list[i].pid, list[i].ppid, list[i].pgid,
                   (unsigned long)list[i].pages, state, list[i].name);
        else
            printf("%5u %-7s %s\n", list[i].pid, state, list[i].name);
    }
    return 0;
}
