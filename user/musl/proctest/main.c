#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-52s ok\n", what);
    else
        printf("  %-52s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static int slurp(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int len = 0;
    ssize_t n;
    while (len < size - 1 && (n = read(fd, buf + len, (size_t)(size - 1 - len))) > 0)
        len += (int)n;
    close(fd);
    buf[len] = 0;
    return len;
}

static long field(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static int maps_line(const char *maps, unsigned long addr, char *perms)
{
    for (const char *line = maps; line && *line;) {
        unsigned long a, b;
        char pr[5];
        if (sscanf(line, "%lx-%lx %4s", &a, &b, pr) == 3 && addr >= a && addr < b) {
            memcpy(perms, pr, 5);
            return 1;
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 0;
}

static int has_name(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, name) == 0)
            found = 1;
    closedir(d);
    return found;
}

int main(int argc, char **argv)
{
    (void)argc;
    printf("\n");
    static char buf[16384];
    char path[128], perms[5];

    check("status de self", slurp("/proc/self/status", buf, sizeof(buf)) > 0);
    check("Pid do status e o getpid", field(buf, "Pid:") == getpid());
    check("PPid do status e o getppid", field(buf, "PPid:") == getppid());
    check("Name e proctest", strstr(buf, "Name:\tproctest") != NULL);

    int n = slurp("/proc/self/cmdline", buf, sizeof(buf));
    check("cmdline tem argv separado por NUL",
          n > 0 && strcmp(buf, argv[0]) == 0 && strcmp(buf + strlen(buf) + 1, "um") == 0);

    char *anon = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    slurp("/proc/self/maps", buf, sizeof(buf));
    check("maps mostra o mmap anonimo rw-p", anon != MAP_FAILED && maps_line(buf, (unsigned long)anon, perms) &&
                                             strcmp(perms, "rw-p") == 0);
    check("maps mostra a pilha", strstr(buf, "[stack]") != NULL);
    check("maps mostra o executavel", strstr(buf, "/bin/proctest") != NULL);
    int sp;
    check("a pilha contem uma variavel local", maps_line(buf, (unsigned long)&sp, perms) && perms[0] == 'r');
    mprotect(anon + 4096, 4096, PROT_READ);
    slurp("/proc/self/maps", buf, sizeof(buf));
    check("mprotect divide a area no maps", maps_line(buf, (unsigned long)anon + 4096, perms) &&
                                            strcmp(perms, "r--p") == 0 &&
                                            maps_line(buf, (unsigned long)anon, perms) && strcmp(perms, "rw-p") == 0);
    munmap(anon, 3 * 4096);
    slurp("/proc/self/maps", buf, sizeof(buf));
    check("munmap tira do maps", !maps_line(buf, (unsigned long)anon, perms));

    int fd = open("/bin/hello", O_RDONLY);
    if (fd >= 0 && dup2(fd, 77) == 77) {
        close(fd);
        fd = 77;
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    char fdname[16];
    snprintf(fdname, sizeof(fdname), "%d", fd);
    check("fd aberto aparece em /proc/self/fd", fd >= 0 && has_name("/proc/self/fd", fdname));
    check("e le o caminho dele", slurp(path, buf, sizeof(buf)) > 0 && strcmp(buf, "/bin/hello\n") == 0);
    close(fd);
    check("fd fechado some", !has_name("/proc/self/fd", fdname));

    char me[16];
    snprintf(me, sizeof(me), "%d", getpid());
    check("/proc lista o proprio pid", has_name("/proc", me));
    struct stat a, b;
    check("/proc/self/.. e /proc", stat("/proc/self/..", &a) == 0 && stat("/proc", &b) == 0 &&
                                    a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    check("escrever em /proc da EROFS", open("/proc/meminfo", O_WRONLY) < 0 && errno == EROFS);
    check("criar em /proc da EROFS", open("/proc/x", O_CREAT | O_WRONLY, 0644) < 0 && errno == EROFS);
    check("pid inexistente da ENOENT", stat("/proc/999999", &a) < 0 && errno == ENOENT);

    pid_t kid = fork();
    if (kid == 0)
        _exit(3);
    usleep(300000);
    snprintf(path, sizeof(path), "/proc/%d/status", kid);
    check("filho morto aparece como zumbi", slurp(path, buf, sizeof(buf)) > 0 && strstr(buf, "State:\tZ"));
    int st;
    waitpid(kid, &st, 0);
    snprintf(path, sizeof(path), "/proc/%d", kid);
    check("depois do wait o diretorio some", stat(path, &a) < 0 && errno == ENOENT);

    kid = fork();
    if (kid == 0) {
        pause();
        _exit(0);
    }
    snprintf(path, sizeof(path), "/proc/%d/maps", kid);
    int mfd = open(path, O_RDONLY);
    kill(kid, SIGKILL);
    waitpid(kid, &st, 0);
    n = mfd >= 0 ? (int)read(mfd, buf, sizeof(buf) - 1) : -1;
    check("maps aberto antes do filho morrer ainda le", n > 0);
    if (mfd >= 0)
        close(mfd);

    check("meminfo tem MemTotal", slurp("/proc/meminfo", buf, sizeof(buf)) > 0 && field(buf, "MemTotal:") > 0);
    check("mounts tem /proc", slurp("/proc/mounts", buf, sizeof(buf)) > 0 && strstr(buf, "proc /proc proc ro"));
    check("cpuinfo tem processor", slurp("/proc/cpuinfo", buf, sizeof(buf)) > 0 && strstr(buf, "processor"));
    check("uptime cresce", slurp("/proc/uptime", buf, sizeof(buf)) > 0 && strtod(buf, NULL) > 0.0);

    printf("proctest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
