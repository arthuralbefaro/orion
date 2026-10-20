#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int inc = 10;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-n")) {
        if (i + 1 >= argc) {
            fprintf(stderr, "uso: nice [-n N] [comando [argumentos]]\n");
            return 125;
        }
        inc = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, 0);
        if (cur == -1 && errno) {
            fprintf(stderr, "nice: getpriority: %s\n", strerror(errno));
            return 125;
        }
        printf("%d\n", cur);
        return 0;
    }
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, 0);
    if (setpriority(PRIO_PROCESS, 0, cur + inc) < 0)
        fprintf(stderr, "nice: nao consegui mudar a prioridade para %d: %s\n", cur + inc, strerror(errno));
    execvp(argv[i], argv + i);
    fprintf(stderr, "nice: %s: %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
