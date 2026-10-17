#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <orion.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    struct dirent *e;
    char full[512];
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR) {
            printf("  <DIR>          %s/\n", e->d_name);
            continue;
        }
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        long size = stat(full, &st) == 0 ? (long)st.st_size : 0;
        printf("  %10lu     %s\n", (unsigned long)size, e->d_name);
    }
    closedir(d);
    return 0;
}
