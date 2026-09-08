#pragma once

#include <sys/types.h>

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

struct stat {
    uint64_t st_size;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint64_t st_ino;
    uint64_t st_blksize;
    uint64_t st_mtime;
};

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
