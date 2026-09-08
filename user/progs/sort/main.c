#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 2048
#define LINE_MAX  512

static char *lines[MAX_LINES];
static int count;
static int reverse;

static void load(FILE *f)
{
    char buf[LINE_MAX];
    while (count < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
        char *copy = malloc(strlen(buf) + 1);
        if (!copy)
            return;
        strcpy(copy, buf);
        lines[count++] = copy;
    }
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        reverse = 1;
        first = 2;
    }
    if (argc <= first) {
        load(stdin);
    } else {
        for (int i = first; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) {
                fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
                return 1;
            }
            load(f);
            fclose(f);
        }
    }

    for (int i = 1; i < count; i++) {
        char *key = lines[i];
        int j = i - 1;
        while (j >= 0 && (reverse ? strcmp(lines[j], key) < 0 : strcmp(lines[j], key) > 0)) {
            lines[j + 1] = lines[j];
            j--;
        }
        lines[j + 1] = key;
    }

    for (int i = 0; i < count; i++)
        printf("%s\n", lines[i]);
    return 0;
}
