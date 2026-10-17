#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <orion.h>

static void count(FILE *f, const char *name)
{
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0, c;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n')
            lines++;
        if (isspace(c))
            in_word = 0;
        else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    printf("%7ld %7ld %7ld %s\n", lines, words, bytes, name);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        count(stdin, "");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], orion_strerror(errno));
            continue;
        }
        count(f, argv[i]);
        fclose(f);
    }
    return 0;
}
