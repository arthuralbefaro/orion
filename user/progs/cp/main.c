#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", from, strerror(errno));
        return 1;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", to, strerror(errno));
        close(in);
        return 1;
    }
    char buf[4096];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            fprintf(stderr, "cp: %s: %s\n", to, strerror(errno));
            rc = 1;
            break;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: %s: %s\n", from, strerror(errno));
        rc = 1;
    }
    close(in);
    close(out);
    return rc;
}

static void join(const char *dir, const char *path, char *out, size_t size)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            base = p + 1;
    snprintf(out, size, "%s/%s", dir, base);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "uso: cp <origem>... <destino>\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct stat st;
    int dest_is_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);

    if (argc > 3 && !dest_is_dir) {
        fprintf(stderr, "cp: %s nao e um diretorio\n", dest);
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[512];
        if (dest_is_dir)
            join(dest, argv[i], target, sizeof(target));
        else
            snprintf(target, sizeof(target), "%s", dest);
        rc |= copy_file(argv[i], target);
    }
    return rc;
}
