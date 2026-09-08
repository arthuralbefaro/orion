#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_OUT 8

int main(int argc, char **argv)
{
    int fds[MAX_OUT];
    int n = 0;
    int first = 1;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        first = 2;
    }
    for (int i = first; i < argc && n < MAX_OUT; i++) {
        int fd = open(argv[i], flags);
        if (fd < 0) {
            fprintf(stderr, "tee: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        fds[n++] = fd;
    }

    char buf[4096];
    long r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, r);
        for (int i = 0; i < n; i++)
            write(fds[i], buf, r);
    }
    for (int i = 0; i < n; i++)
        close(fds[i]);
    return 0;
}
