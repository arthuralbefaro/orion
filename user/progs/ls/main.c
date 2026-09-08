#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_is_dir)
            printf("  <DIR>          %s/\n", e->d_name);
        else
            printf("  %10lu     %s\n", e->d_size, e->d_name);
    }
    closedir(d);
    return 0;
}
