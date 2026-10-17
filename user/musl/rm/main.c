#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orion.h>

static int recursive;
static int failures;

static int remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) < 0) {
            fprintf(stderr, "rm: %s: %s\n", path, orion_strerror(errno));
            return 1;
        }
        return 0;
    }
    if (!recursive) {
        fprintf(stderr, "rm: %s: e um diretorio (use -r)\n", path);
        return 1;
    }

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "rm: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        failures += remove_tree(child);
    }
    closedir(d);

    if (rmdir(path) < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        recursive = 1;
        first = 2;
    }
    if (argc <= first) {
        fprintf(stderr, "uso: rm [-r] <arquivo>...\n");
        return 1;
    }
    for (int i = first; i < argc; i++)
        failures += remove_tree(argv[i]);
    return failures ? 1 : 0;
}
