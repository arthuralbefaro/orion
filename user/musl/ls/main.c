#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <orion.h>

static int longfmt;
static time_t now;

static void mode_string(mode_t m, char *s)
{
    static const char types[] = "?pc?d?b?-?l?s???";
    s[0] = types[(m >> 12) & 15];
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s[1 + i] = (m & (0400u >> i)) ? rwx[i] : '-';
    if (m & S_ISUID)
        s[3] = (m & S_IXUSR) ? 's' : 'S';
    if (m & S_ISGID)
        s[6] = (m & S_IXGRP) ? 's' : 'S';
    if (m & S_ISVTX)
        s[9] = (m & S_IXOTH) ? 't' : 'T';
    s[10] = 0;
}

static const char *user_name(uid_t uid, char *buf, size_t size)
{
    struct passwd *pw = getpwuid(uid);
    if (pw)
        return pw->pw_name;
    snprintf(buf, size, "%u", (unsigned)uid);
    return buf;
}

static const char *group_name(gid_t gid, char *buf, size_t size)
{
    struct group *gr = getgrgid(gid);
    if (gr)
        return gr->gr_name;
    snprintf(buf, size, "%u", (unsigned)gid);
    return buf;
}

static void show_long(const char *path, const char *name, const struct stat *st)
{
    char mode[11], ubuf[16], gbuf[16], when[32];
    mode_string(st->st_mode, mode);
    time_t t = st->st_mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    long long age = (long long)now - (long long)t;
    if (age > 180LL * 24 * 3600 || age < -3600)
        strftime(when, sizeof(when), "%b %e  %Y", &tm);
    else
        strftime(when, sizeof(when), "%b %e %H:%M", &tm);
    printf("%s %3lu %-8s %-8s %8lld %s %s", mode, (unsigned long)st->st_nlink,
           user_name(st->st_uid, ubuf, sizeof(ubuf)), group_name(st->st_gid, gbuf, sizeof(gbuf)),
           (long long)st->st_size, when, name);
    if (S_ISLNK(st->st_mode)) {
        char target[512];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = 0;
            printf(" -> %s", target);
        }
    }
    putchar('\n');
}

static void show_short(const char *path, const char *name, int is_dir)
{
    if (is_dir) {
        printf("  <DIR>          %s/\n", name);
        return;
    }
    struct stat st;
    long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    printf("  %10lu     %s\n", (unsigned long)size, name);
}

static int list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    struct dirent *e;
    char full[512];
    while ((e = readdir(d))) {
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (!longfmt) {
            show_short(full, e->d_name, e->d_type == DT_DIR);
            continue;
        }
        struct stat st;
        if (lstat(full, &st) < 0) {
            fprintf(stderr, "ls: %s: %s\n", full, orion_strerror(errno));
            continue;
        }
        show_long(full, e->d_name, &st);
    }
    closedir(d);
    return 0;
}

static int list(const char *path, int header)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (header)
            printf("%s:\n", path);
        return list_dir(path);
    }
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, orion_strerror(errno));
        return 1;
    }
    if (longfmt)
        show_long(path, path, &st);
    else
        show_short(path, path, 0);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o != 'l') {
                fprintf(stderr, "ls: opcao invalida -%c\nuso: ls [-l] [caminho...]\n", *o);
                return 2;
            }
            longfmt = 1;
        }
    }
    now = time(NULL);
    if (i == argc)
        return list(".", 0);
    int status = 0, many = argc - i > 1;
    for (; i < argc; i++)
        status |= list(argv[i], many);
    return status;
}
