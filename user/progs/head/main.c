#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show(FILE *f, long limit)
{
    char line[1024];
    for (long i = 0; i < limit && fgets(line, sizeof(line), f); i++)
        fputs(line, stdout);
}

static void show_bytes(FILE *f, long limit)
{
    char buf[512];
    while (limit > 0) {
        size_t want = limit < (long)sizeof(buf) ? (size_t)limit : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0)
            break;
        fwrite(buf, 1, n, stdout);
        limit -= (long)n;
    }
}

int main(int argc, char **argv)
{
    long limit = 10;
    int first = 1;
    int bytes = 0;
    if (argc > 2 && (strcmp(argv[1], "-n") == 0 || strcmp(argv[1], "-c") == 0)) {
        bytes = argv[1][1] == 'c';
        limit = atoi(argv[2]);
        first = 3;
    }
    if (argc <= first) {
        if (bytes)
            show_bytes(stdin, limit);
        else
            show(stdin, limit);
        return 0;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (argc - first > 1)
            printf("==> %s <==\n", argv[i]);
        if (bytes)
            show_bytes(f, limit);
        else
            show(f, limit);
        fclose(f);
    }
    return rc;
}
