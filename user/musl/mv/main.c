#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orion.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "uso: mv <origem> <destino>\n");
        return 1;
    }
    if (rename(argv[1], argv[2]) < 0) {
        fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], argv[2], orion_strerror(errno));
        return 1;
    }
    return 0;
}
