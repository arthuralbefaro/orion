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

int main(int argc, char **argv)
{
    long limit = 10;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        limit = atoi(argv[2]);
        first = 3;
    }
    if (argc <= first) {
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
        show(f, limit);
        fclose(f);
    }
    return rc;
}
