#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

DIR *opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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
    if (d->pos >= d->len) {
        long n = syscall3(SYS_GETDENTS64, d->fd, (long)d->buf, sizeof(d->buf));
        if (n <= 0) {
            if (n < 0)
                errno = (int)-n;
            return NULL;
        }
        d->len = (int)n;
        d->pos = 0;
    }
    struct dirent *raw = (struct dirent *)(d->buf + d->pos);
    d->pos += raw->d_reclen;
    d->entry.d_ino = raw->d_ino;
    d->entry.d_off = raw->d_off;
    d->entry.d_reclen = raw->d_reclen;
    d->entry.d_type = raw->d_type;
    size_t max = raw->d_reclen - (size_t)((char *)raw->d_name - (char *)raw);
    size_t n = strnlen(raw->d_name, max);
    if (n >= sizeof(d->entry.d_name))
        n = sizeof(d->entry.d_name) - 1;
    memcpy(d->entry.d_name, raw->d_name, n);
    d->entry.d_name[n] = '\0';
    return &d->entry;
}

void rewinddir(DIR *d)
{
    lseek(d->fd, 0, SEEK_SET);
    d->pos = d->len = 0;
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}
