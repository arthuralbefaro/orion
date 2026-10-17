#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <orion.h>

static int invert;
static int show_names;
static int matches;

static int contains(const char *line, const char *needle)
{
    return strstr(line, needle) != NULL;
}

static void scan(FILE *f, const char *needle, const char *name)
{
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        int hit = contains(line, needle);
        if (invert)
            hit = !hit;
        if (!hit)
            continue;
        matches++;
        if (show_names)
            printf("%s:", name);
        fputs(line, stdout);
        if (!strchr(line, '\n'))
            putchar('\n');
    }
}

int main(int argc, char **argv)
{
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        if (strcmp(argv[first], "-v") == 0)
            invert = 1;
        else
            break;
        first++;
    }
    if (argc <= first) {
        fprintf(stderr, "uso: grep [-v] <texto> [arquivo...]\n");
        return 2;
    }
    const char *needle = argv[first++];

    if (argc <= first) {
        scan(stdin, needle, "-");
        return matches ? 0 : 1;
    }
    show_names = argc - first > 1;
    int rc = 0;
    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "grep: %s: %s\n", argv[i], orion_strerror(errno));
            rc = 2;
            continue;
        }
        scan(f, needle, argv[i]);
        fclose(f);
    }
    if (rc)
        return rc;
    return matches ? 0 : 1;
}
