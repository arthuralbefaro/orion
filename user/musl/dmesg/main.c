#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("uso: dmesg [-g texto]\n");
            printf("  mostra o log do kernel (os ultimos 64 KiB, de /proc/kmsg)\n");
            printf("  -g texto  so as linhas que contem o texto\n");
            return 0;
        }
        if (!strcmp(argv[i], "-g") && i + 1 < argc)
            only = argv[++i];
    }
    FILE *f = fopen("/proc/kmsg", "r");
    if (!f) {
        perror("dmesg: /proc/kmsg");
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (only && !strstr(line, only))
            continue;
        fputs(line, stdout);
    }
    fclose(f);
    return 0;
}
