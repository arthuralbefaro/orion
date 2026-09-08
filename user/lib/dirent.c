#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "syscall.h"

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    DIR *d = calloc(1, sizeof(*d));
    if (!d) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d)
{
    long r = syscall3(SYS_READDIR, d->fd, (long)&d->entry, (long)d->index);
    if (r <= 0)
        return NULL;
    d->index++;
    return &d->entry;
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}
