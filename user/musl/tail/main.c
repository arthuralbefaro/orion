#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orion.h>

#define MAX_KEEP 64
#define LINE_MAX 512

static void show(FILE *f, long limit)
{
    static char ring[MAX_KEEP][LINE_MAX];
    long count = 0;
    if (limit > MAX_KEEP)
        limit = MAX_KEEP;

    char line[LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        snprintf(ring[count % limit], LINE_MAX, "%s", line);
        count++;
    }
    long start = count > limit ? count - limit : 0;
    for (long i = start; i < count; i++)
        fputs(ring[i % limit], stdout);
}

int main(int argc, char **argv)
{
    long limit = 10;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        limit = atoi(argv[2]);
        first = 3;
    }
    if (limit <= 0)
        limit = 1;
    if (argc <= first) {
        show(stdin, limit);
        return 0;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], orion_strerror(errno));
            rc = 1;
            continue;
        }
        if (argc - first > 1)
            printf("==> %s <==\n", argv[i]);
        show(f, limit);
        fclose(f);
    }
    return rc;
}
