#pragma once

#include <sys/types.h>

struct dirent {
    uint64_t d_size;
    uint8_t d_is_dir;
    char d_name[256];
};

typedef struct {
    int fd;
    unsigned long index;
    struct dirent entry;
} DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
