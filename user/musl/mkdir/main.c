#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orion.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: mkdir <diretorio>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], orion_strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
