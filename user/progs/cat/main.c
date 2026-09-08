#include <stdio.h>
#include <string.h>
#include <errno.h>

static int dump(FILE *f)
{
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, stdout);
    return ferror(f) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return dump(stdin);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (dump(f))
            rc = 1;
        fclose(f);
    }
    return rc;
}
