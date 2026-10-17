#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <orion.h>

#define LINE_MAX 512

static int show_count;

static void run(FILE *f)
{
    char prev[LINE_MAX] = "";
    char line[LINE_MAX];
    int have = 0;
    long repeats = 0;

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (have && strcmp(prev, line) == 0) {
            repeats++;
            continue;
        }
        if (have) {
            if (show_count)
                printf("%7ld %s\n", repeats, prev);
            else
                printf("%s\n", prev);
        }
        snprintf(prev, sizeof(prev), "%s", line);
        repeats = 1;
        have = 1;
    }
    if (have) {
        if (show_count)
            printf("%7ld %s\n", repeats, prev);
        else
            printf("%s\n", prev);
    }
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        show_count = 1;
        first = 2;
    }
    if (argc <= first) {
        run(stdin);
        return 0;
    }
    FILE *f = fopen(argv[first], "r");
    if (!f) {
        fprintf(stderr, "uniq: %s: %s\n", argv[first], orion_strerror(errno));
        return 1;
    }
    run(f);
    fclose(f);
    return 0;
}
