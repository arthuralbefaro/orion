#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define AREA (16 * 1024)

static const char *disk;
static int fails;

static long flushes(void)
{
    FILE *f = fopen("/proc/diskstats", "r");
    if (!f)
        return -1;
    char line[256];
    long r = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned maj, min;
        char name[32];
        long v[17];
        int n = sscanf(line, "%u %u %31s %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld", &maj,
                       &min, name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10],
                       &v[11], &v[12], &v[13], &v[14], &v[15], &v[16]);
        if (n == 20 && strcmp(name, disk) == 0)
            r = v[15];
    }
    fclose(f);
    return r;
}

static void check(int ok, const char *what)
{
    printf("flushtest: %s: %s\n", what, ok ? "OK" : "FALHOU");
    if (!ok)
        fails++;
}

static int do_fsync(int fd)
{
    return fsync(fd);
}

static int do_fdatasync(int fd)
{
    return fdatasync(fd);
}

static int do_syncfs(int fd)
{
    return syncfs(fd);
}

static int do_sync(int fd)
{
    (void)fd;
    sync();
    return 0;
}

static void sync_ops(int fd)
{
    static const struct {
        const char *name;
        int (*fn)(int);
    } ops[] = { { "fsync", do_fsync }, { "fdatasync", do_fdatasync }, { "syncfs", do_syncfs }, { "sync", do_sync } };
    char buf[1024];
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        memset(buf, 'a' + (int)i, sizeof(buf));
        pwrite(fd, buf, sizeof(buf), 0);
        long before = flushes();
        int r = ops[i].fn(fd);
        int e = errno;
        long after = flushes();
        char what[96];
        snprintf(what, sizeof(what), "%s devolve 0 e manda FLUSH ao %s (%ld -> %ld)", ops[i].name, disk, before, after);
        if (r != 0)
            printf("flushtest: %s: %s\n", ops[i].name, strerror(e));
        check(r == 0 && before >= 0 && after > before, what);
    }
    sync();
    long idle0 = flushes();
    sleep(5);
    long idle1 = flushes();
    char idle[96];
    snprintf(idle, sizeof(idle), "5 s parado sem escrita: o bdflush nao manda FLUSH (%ld -> %ld)", idle0, idle1);
    check(idle0 >= 0 && idle1 == idle0, idle);
    errno = 0;
    int r = fsync(9999);
    check(r < 0 && errno == EBADF, "fsync num fd fechado da EBADF");
    errno = 0;
    r = fdatasync(9999);
    check(r < 0 && errno == EBADF, "fdatasync num fd fechado da EBADF");
}

static int same(const unsigned char *p, size_t len, unsigned char c)
{
    for (size_t i = 0; i < len; i++)
        if (p[i] != c)
            return 0;
    return 1;
}

static int expect_layout(const unsigned char *p, const char *layout, size_t unit)
{
    for (size_t i = 0; layout[i]; i++)
        if (!same(p + i * unit, unit, (unsigned char)layout[i]))
            return 0;
    return 1;
}

static int file_layout(int fd, const char *layout, size_t unit)
{
    static unsigned char buf[AREA];
    size_t len = strlen(layout) * unit;
    if (pread(fd, buf, len, 0) != (ssize_t)len)
        return 0;
    return expect_layout(buf, layout, unit);
}

static void fill(int fd, off_t off, size_t len, unsigned char c)
{
    static unsigned char buf[AREA];
    memset(buf, c, len);
    if (pwrite(fd, buf, len, off) != (ssize_t)len)
        printf("flushtest: escrita de %zu bytes em %ld: %s\n", len, (long)off, strerror(errno));
}

static int file_mode(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("flushtest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    sync_ops(fd);

    fill(fd, 0, AREA, 'a');
    fsync(fd);
    check(file_layout(fd, "aaaa", 4096), "arquivo: 16 KiB lidos de volta (cache de paginas cheio)");
    unsigned char *map = mmap(NULL, AREA, PROT_READ, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED && expect_layout(map, "aaaa", 4096), "arquivo: mmap ve os 16 KiB");
    fill(fd, 4096, 8192, 'b');
    check(file_layout(fd, "abba", 4096), "arquivo: escrita de 8 KiB lida logo depois pelo read");
    check(map != MAP_FAILED && expect_layout(map, "abba", 4096), "arquivo: escrita de 8 KiB vista pelo mmap");
    fill(fd, 5000, 100, 'c');
    unsigned char buf[AREA];
    int ok = pread(fd, buf, AREA, 0) == AREA && same(buf + 4096, 904, 'b') && same(buf + 5000, 100, 'c') &&
             same(buf + 5100, 7188, 'b') && same(buf, 4096, 'a') && same(buf + 12288, 4096, 'a');
    check(ok, "arquivo: escrita pequena por cima da grande lida logo depois");
    fsync(fd);
    if (map != MAP_FAILED)
        munmap(map, AREA);
    close(fd);
    fd = open(path, O_RDONLY);
    ok = fd >= 0 && pread(fd, buf, AREA, 0) == AREA && same(buf + 5000, 100, 'c') && same(buf + 8192, 4096, 'b');
    check(ok, "arquivo: reaberto, o dado novo continua la");
    if (fd >= 0)
        close(fd);
    unlink(path);
    return 0;
}

static int dev_layout(int fd, off_t base, const char *layout, size_t unit)
{
    static unsigned char buf[AREA];
    size_t len = strlen(layout) * unit;
    if (lseek(fd, base, SEEK_SET) != base || read(fd, buf, len) != (ssize_t)len)
        return 0;
    return expect_layout(buf, layout, unit);
}

static void dev_fill(int fd, off_t off, size_t len, unsigned char c)
{
    static unsigned char buf[AREA];
    memset(buf, c, len);
    if (lseek(fd, off, SEEK_SET) != off || write(fd, buf, len) != (ssize_t)len)
        printf("flushtest: escrita de %zu bytes em %ld: %s\n", len, (long)off, strerror(errno));
}

static int block_mode(const char *dev)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        printf("flushtest: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    off_t base = 2048 * 512;
    dev_fill(fd, base, 4096, '0');
    fsync(fd);
    dev_fill(fd, base, 512, 'x');
    check(dev_layout(fd, base, "x", 512), "disco: setor escrito sozinho (fica no cache) lido de volta");
    dev_fill(fd, base, 4096, 'y');
    check(dev_layout(fd, base, "y", 512), "disco: escrita direta de 4 KiB atualiza o setor que estava no cache");
    check(dev_layout(fd, base, "y", 4096), "disco: 4 KiB lidos direto do disco depois da escrita direta");
    dev_fill(fd, base + 512, 512, 'z');
    check(dev_layout(fd, base, "yzyyyyyy", 512), "disco: setor sujo no cache no meio de 4 KiB lidos");
    dev_fill(fd, base, 4096, 'w');
    check(dev_layout(fd, base + 512, "w", 512), "disco: escrita direta por cima de um setor sujo do cache");
    long before = flushes();
    int r = fsync(fd);
    long after = flushes();
    char what[96];
    snprintf(what, sizeof(what), "disco: fsync no dispositivo manda FLUSH (%ld -> %ld)", before, after);
    check(r == 0 && after > before, what);
    check(dev_layout(fd, base, "w", 4096), "disco: depois do fsync o setor sujo antigo nao voltou por cima");
    close(fd);
    return 0;
}

static int command_mode(char **argv)
{
    long before = flushes();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    long after = flushes();
    char what[160];
    snprintf(what, sizeof(what), "%s manda FLUSH ao %s (%ld -> %ld)", argv[0], disk, before, after);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0 && after > before, what);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 4 && strcmp(argv[1], "-f") == 0) {
        disk = argv[2];
        file_mode(argv[3]);
    } else if (argc >= 4 && strcmp(argv[1], "-b") == 0) {
        disk = argv[3];
        block_mode(argv[2]);
    } else if (argc >= 4 && strcmp(argv[1], "-c") == 0) {
        disk = argv[2];
        command_mode(argv + 3);
    } else {
        fprintf(stderr, "uso: flushtest -f <disco> <arquivo> | -b <dispositivo> <disco> | -c <disco> <comando...>\n");
        return 2;
    }
    printf("flushtest: fim, %d falha(s)\n", fails);
    return fails ? 1 : 0;
}
