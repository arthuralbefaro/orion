#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: sleep <segundos>\n");
        return 1;
    }
    int secs = atoi(argv[1]);
    for (int i = 0; i < secs; i++)
        usleep(1000000);
    return 0;
}
