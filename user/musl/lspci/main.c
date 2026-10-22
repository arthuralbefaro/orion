#include <stdio.h>
#include <string.h>

int main(void)
{
    FILE *f = fopen("/proc/pci", "r");
    if (!f) {
        perror("lspci: /proc/pci");
        return 1;
    }
    char line[256];
    int n = 0;
    printf("%-8s %-9s %-7s %-11s %s\n", "endereco", "id", "classe", "driver", "tipo");
    while (fgets(line, sizeof(line), f)) {
        char addr[16], id[16], cls[16], drv[32], kind[64] = "";
        if (sscanf(line, "%15s %15s %15s %31s %63[^\n]", addr, id, cls, drv, kind) < 4)
            continue;
        printf("%-8s %-9s %-7s %-11s %s\n", addr, id, cls, drv, kind);
        n++;
    }
    fclose(f);
    printf("%d dispositivo(s)\n", n);
    return 0;
}
