#include <errno.h>
#include <fcntl.h>
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

static char *slurp(const char *path, long *size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    fstat(fd, &st);
    char *buf = malloc(st.st_size ? st.st_size : 1);
    long done = 0;
    ssize_t n;
    while (buf && done < st.st_size && (n = read(fd, buf + done, (size_t)(st.st_size - done))) > 0)
        done += n;
    close(fd);
    *size = done;
    return buf;
}

static long disk_reads(const char *dev)
{
    char buf[2048];
    int fd = open("/proc/diskstats", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    for (char *line = buf; line && *line;) {
        unsigned maj, min;
        char name[32];
        long reads;
        if (sscanf(line, "%u %u %31s %ld", &maj, &min, name, &reads) == 4 && strcmp(name, dev) == 0)
            return reads;
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return -1;
}

static long cached_kb(void)
{
    char buf[1024];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    char *p = strstr(buf, "Cached:");
    return p ? strtol(p + 7, NULL, 10) : -1;
}

static void test_read(const char *file, const char *dev)
{
    long size1 = 0, size2 = 0;
    long c0 = cached_kb();
    char *a = slurp(file, &size1);
    long r0 = disk_reads(dev);
    char *b = slurp(file, &size2);
    long r1 = disk_reads(dev);
    printf("  %s: %ld bytes; leituras do disco %s: %ld antes, %ld depois da segunda\n", file, size1, dev, r0, r1);
    check("leu o arquivo duas vezes", a && b && size1 > 100000 && size1 == size2);
    check("as duas leituras sao iguais", a && b && memcmp(a, b, (size_t)size1) == 0);
    check("a segunda leitura nao vai ao disco", r0 >= 0 && r1 == r0);
    check("Cached cresceu", cached_kb() - c0 >= size1 / 1024 - 8 || cached_kb() >= size1 / 1024);

    int fd = open(file, O_RDONLY);
    char *m = fd >= 0 ? mmap(NULL, (size_t)size1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    check("mmap MAP_PRIVATE do arquivo", m != MAP_FAILED);
    if (m != MAP_FAILED) {
        check("o mmap mostra o mesmo conteudo do read", memcmp(m, a, (size_t)size1) == 0);
        long r2 = disk_reads(dev);
        check("e nao foi ao disco para isso", r2 == r1);
        m[5000] ^= 0x5A;
        char c;
        check("escrever no mmap nao muda o arquivo",
              pread(fd, &c, 1, 5000) == 1 && c == a[5000] && m[5000] == (char)(a[5000] ^ 0x5A));
        pid_t kid = fork();
        if (kid == 0)
            _exit(memcmp(m + 8192, a + 8192, 4096) == 0 && m[5000] == (char)(a[5000] ^ 0x5A) ? 0 : 1);
        int st = 1;
        waitpid(kid, &st, 0);
        check("o filho ve o mmap do pai", st == 0);
        munmap(m, (size_t)size1);
    }
    if (fd >= 0)
        close(fd);
    free(a);
    free(b);
}

static void test_write(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/pc.dat", dir);
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char page[4096];
    for (int i = 0; i < 3; i++) {
        memset(page, 'a' + i, sizeof(page));
        write(fd, page, sizeof(page));
    }
    char c = 0;
    check("arquivo de 3 paginas", fd >= 0 && pread(fd, &c, 1, 4096 + 7) == 1 && c == 'b');
    char *m = mmap(NULL, 3 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap so leitura", m != MAP_FAILED);
    pwrite(fd, "XYZ", 3, 4096 + 100);
    check("read ve o pwrite", pread(fd, page, 3, 4096 + 100) == 3 && memcmp(page, "XYZ", 3) == 0);
    check("o mmap ja feito ve o pwrite", m != MAP_FAILED && memcmp(m + 4096 + 100, "XYZ", 3) == 0);
    check("ftruncate para 5000", ftruncate(fd, 5000) == 0);
    struct stat st;
    check("read para no novo fim", fstat(fd, &st) == 0 && st.st_size == 5000 &&
                                   pread(fd, page, 4096, 4096) == 904);
    check("ftruncate de volta para 9000 le zeros", ftruncate(fd, 9000) == 0 && pread(fd, page, 10, 6000) == 10 &&
                                                   page[0] == 0 && page[9] == 0);
    if (m != MAP_FAILED)
        munmap(m, 3 * 4096);
    close(fd);
    check("unlink do arquivo com cache", unlink(path) == 0);
}

int main(int argc, char **argv)
{
    printf("\n");
    const char *dir = NULL;
    const char *file = "/bin/doom";
    const char *dev = "sda";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            dir = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dev = argv[++i];
        else
            file = argv[i];
    }
    printf("pctest: leitura e mmap de %s\n", file);
    test_read(file, dev);
    if (dir) {
        printf("pctest: escrita e truncate em %s\n", dir);
        test_write(dir);
    }
    printf("pctest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
