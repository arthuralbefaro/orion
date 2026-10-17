#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <orion.h>

static int usage(void)
{
    fprintf(stderr, "uso: mount [-r] [-t tipo] [-o ro|rw] <origem> <destino>\n");
    return 1;
}

static int list(void)
{
    FILE *m = fopen("/proc/mounts", "r");
    if (!m) {
        fprintf(stderr, "mount: /proc/mounts: %s\n", orion_strerror(errno));
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), m))
        fputs(line, stdout);
    fclose(m);
    return 0;
}

int main(int argc, char **argv)
{
    const char *type = NULL;
    unsigned long flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            flags |= MS_RDONLY;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            const char *o = argv[++i];
            if (strcmp(o, "ro") == 0)
                flags |= MS_RDONLY;
            else if (strcmp(o, "rw") != 0)
                return usage();
        } else {
            return usage();
        }
    }
    if (i == argc)
        return list();
    if (argc - i != 2)
        return usage();
    const char *src = argv[i], *dst = argv[i + 1];
    if (!type)
        type = strncmp(src, "/dev/", 5) == 0 ? "vfat" : src;
    if (mount(src, dst, type, flags, NULL) < 0) {
        fprintf(stderr, "mount: %s em %s: %s\n", src, dst, orion_strerror(errno));
        return 1;
    }
    return 0;
}
