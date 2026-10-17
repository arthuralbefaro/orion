#include <errno.h>
#include <stdio.h>
#include <sys/mount.h>
#include <orion.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: umount <destino>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (umount(argv[i]) < 0) {
            fprintf(stderr, "umount: %s: %s\n", argv[i], orion_strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
