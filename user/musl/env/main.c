#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    int i = 1;
    int cleared = 0;
    if (i < argc && (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0)) {
        i++;
        cleared = 1;
    }
    while (i < argc && strchr(argv[i], '=')) {
        putenv(argv[i]);
        i++;
    }
    if (cleared && i >= argc) {
        printf("env: -i sem comando limpa nada visivel\n");
        return 0;
    }
    if (i >= argc) {
        for (char **e = environ; e && *e; e++)
            puts(*e);
        return 0;
    }
    char path[256];
    if (strchr(argv[i], '/')) {
        snprintf(path, sizeof(path), "%s", argv[i]);
    } else {
        snprintf(path, sizeof(path), "/bin/%s", argv[i]);
    }
    execv(path, &argv[i]);
    fprintf(stderr, "env: %s: nao encontrado\n", argv[i]);
    return 127;
}
