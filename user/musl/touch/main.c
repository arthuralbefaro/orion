#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <orion.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: touch <arquivo>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", argv[i], orion_strerror(errno));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
