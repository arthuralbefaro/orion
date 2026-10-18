#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TICKS_PER_SEC 100

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static int find_pid(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[64], buf[512];
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        if (read_text(path, buf, sizeof(buf)) < 0)
            continue;
        char *p = strstr(buf, "Name:");
        if (!p)
            continue;
        p += 5;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t len = strcspn(p, "\n");
        if (len == strlen(name) && strncmp(p, name, len) == 0) {
            found = atoi(e->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

static long cpu_ticks(int pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_text(path, buf, sizeof(buf)) < 0)
        return -1;
    char *p = strrchr(buf, ')');
    if (!p || !p[1])
        return -1;
    p += 2;
    long utime = -1, stime = -1;
    int field = 3;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 14)
            utime = atol(tok);
        if (field == 15) {
            stime = atol(tok);
            break;
        }
    }
    return utime < 0 || stime < 0 ? -1 : utime + stime;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: cpuuso <nome do processo> [segundos]\n");
        return 2;
    }
    int secs = argc > 2 ? atoi(argv[2]) : 10;
    if (secs < 1)
        secs = 1;
    int pid = find_pid(argv[1]);
    if (pid < 0) {
        fprintf(stderr, "cpuuso: nenhum processo chamado %s\n", argv[1]);
        return 1;
    }
    long a = cpu_ticks(pid);
    sleep((unsigned)secs);
    long b = cpu_ticks(pid);
    if (a < 0 || b < 0) {
        fprintf(stderr, "cpuuso: nao li /proc/%d/stat\n", pid);
        return 1;
    }
    long used = b - a;
    long tenths = used * 1000 / ((long)secs * TICKS_PER_SEC);
    printf("cpuuso: %s (pid %d) usou %ld.%ld%% de CPU em %d s (%ld tique(s))\n", argv[1], pid, tenths / 10,
           tenths % 10, secs, used);
    return 0;
}
