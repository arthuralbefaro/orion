#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 2) {
        for (char **e = environ; e && *e; e++)
            puts(*e);
        return 0;
    }
    int missing = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = getenv(argv[i]);
        if (v)
            puts(v);
        else
            missing = 1;
    }
    return missing;
}
