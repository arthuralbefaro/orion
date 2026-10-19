#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orion.h>

static int fails;
static char dir[256];

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-52s ok\n", what);
    else
        printf("  %-52s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static long free_pages(void)
{
    return orion_free_or_cached_pages();
}

static long settled_pages(void)
{
    long best = free_pages();
    for (int i = 0; i < 100; i++) {
        usleep(10000);
        long now = free_pages();
        if (now <= best)
            break;
        best = now;
    }
    return best;
}

static const char *P(const char *rel)
{
    static char bufs[4][512];
    static int n;
    char *b = bufs[n++ & 3];
    snprintf(b, 512, "%s/%s", dir, rel);
    return b;
}

static int make_files(const char *sub, int count, char fill)
{
    char data[3000];
    memset(data, fill, sizeof(data));
    for (int i = 0; i < count; i++) {
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/f%04d", sub, i);
        int fd = open(nm, O_CREAT | O_WRONLY | O_EXCL, 0644);
        if (fd < 0)
            return 0;
        ssize_t w = write(fd, data, sizeof(data));
        close(fd);
        if (w != (ssize_t)sizeof(data))
            return 0;
    }
    return 1;
}

static int remove_files(const char *sub, int count)
{
    int ok = 1;
    for (int i = 0; i < count; i++) {
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/f%04d", sub, i);
        if (unlink(nm) < 0)
            ok = 0;
    }
    return ok;
}

static int file_has(const char *path, char fill, size_t size)
{
    char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    size_t total = 0;
    ssize_t n;
    int ok = 1;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != fill)
                ok = 0;
        total += (size_t)n;
    }
    close(fd);
    return ok && total == size;
}

static void test_thousand(void)
{
    char sub[512];
    strcpy(sub, P("tt"));
    mkdir(sub, 0755);
    long f0 = free_pages();
    check("criou 1000 arquivos de 3000 bytes", make_files(sub, 1000, 'a'));
    long f1 = free_pages();
    check("cada arquivo ocupa uma pagina", f0 - f1 >= 1000);
    char nm[600];
    snprintf(nm, sizeof(nm), "%s/f0500", sub);
    check("conteudo de um deles confere", file_has(nm, 'a', 3000));
    check("apagou os 1000", remove_files(sub, 1000));
    long base = settled_pages();
    check("as paginas voltaram", f0 - base < 256);

    char m0[1024], m1[1024];
    orion_read_text("/proc/meminfo", m0, sizeof(m0));
    check("criou mais 1000", make_files(sub, 1000, 'b'));
    check("apagou de novo", remove_files(sub, 1000));
    long after = settled_pages();
    orion_read_text("/proc/meminfo", m1, sizeof(m1));
    long diff = base - after;
    printf("  paginas livres: %ld antes, %ld depois da segunda rodada\n", base, after);
    printf("  kB: MemFree %ld, Cached %ld, KernelHeap %ld, CowShared %ld\n",
           (long)orion_field(m1, "MemFree") - (long)orion_field(m0, "MemFree"),
           (long)orion_field(m1, "Cached") - (long)orion_field(m0, "Cached"),
           (long)orion_field(m1, "KernelHeap") - (long)orion_field(m0, "KernelHeap"),
           (long)orion_field(m1, "CowShared") - (long)orion_field(m0, "CowShared"));
    check("1000 arquivos criados e apagados sem vazar pagina", diff <= 256 && diff >= -256);
    check("rmdir do diretorio de teste", rmdir(sub) == 0);
}

static void test_big(void)
{
    const char *big = P("grande");
    long f0 = free_pages();
    int fd = open(big, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char buf[4096];
    memset(buf, 'z', sizeof(buf));
    int ok = fd >= 0;
    for (int i = 0; i < 256 && ok; i++)
        ok = write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
    check("escreveu 1 MiB", ok);
    check("le 1 MiB de volta", file_has(big, 'z', 1 << 20));
    check("ftruncate para 0 libera as paginas", fd >= 0 && ftruncate(fd, 0) == 0 && f0 - free_pages() < 16);
    struct stat st;
    check("pwrite longe deixa buraco de zeros",
          fd >= 0 && pwrite(fd, "Q", 1, 100000) == 1 && fstat(fd, &st) == 0 && st.st_size == 100001 &&
              pread(fd, buf, 10, 50000) == 10 && buf[0] == 0 && buf[9] == 0);
    check("o buraco nao ocupa pagina", f0 - free_pages() < 16);
    if (fd >= 0)
        close(fd);
    unlink(big);

    fd = open(big, O_CREAT | O_RDWR | O_TRUNC, 0644);
    memset(buf, 'y', sizeof(buf));
    for (int i = 0; i < 64 && fd >= 0; i++)
        write(fd, buf, sizeof(buf));
    long held = free_pages();
    check("unlink com o arquivo aberto", fd >= 0 && unlink(big) == 0);
    check("as paginas ficam enquanto esta aberto", free_pages() - held < 16);
    check("e ainda le", fd >= 0 && pread(fd, buf, 4096, 4096 * 10) == 4096 && buf[100] == 'y');
    if (fd >= 0)
        close(fd);
    check("fechar libera as paginas", free_pages() - held >= 60);
}

static void test_limit(void)
{
    char lim[512];
    strcpy(lim, P("lim"));
    rmdir(lim);
    mkdir(lim, 0755);
    check("mount tmpfs com size=64k", mount("tmpfs", lim, "tmpfs", 0, "size=64k") == 0);
    char path[600];
    snprintf(path, sizeof(path), "%s/cheio", lim);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    char buf[4096];
    memset(buf, 'k', sizeof(buf));
    long total = 0;
    ssize_t n = 0;
    while (fd >= 0 && (n = write(fd, buf, sizeof(buf))) > 0)
        total += n;
    int err = errno;
    check("escrita para perto de 64 KiB", total >= 49152 && total <= 65536);
    errno = err;
    check("e da ENOSPC", n < 0 && err == ENOSPC);
    if (fd >= 0)
        close(fd);
    check("umount com o tmpfs cheio", unlink(path) == 0 && umount(lim) == 0);
    check("rmdir do ponto de montagem", rmdir(lim) == 0);
}

static void test_review(void)
{
    char m[512];
    strcpy(m, P("rv"));
    rmdir(m);
    mkdir(m, 0755);
    check("mount tmpfs com nr_inodes=40", mount("tmpfs", m, "tmpfs", 0, "nr_inodes=40") == 0);
    int made = 0;
    for (; made < 100; made++) {
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/i%03d", m, made);
        int fd = open(nm, O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            break;
        close(fd);
    }
    check("nr_inodes limita com ENOSPC", made > 30 && made < 40 && errno == ENOSPC);
    for (int i = 0; i < made; i++) {
        char nm[600];
        snprintf(nm, sizeof(nm), "%s/i%03d", m, i);
        unlink(nm);
    }
    check("remonta sem limite de inodes", umount(m) == 0 && mount("tmpfs", m, "tmpfs", 0, NULL) == 0);

    char nm[600];
    snprintf(nm, sizeof(nm), "%s/esparso", m);
    long f0 = free_pages();
    int fd = open(nm, O_CREAT | O_RDWR, 0644);
    struct stat st;
    check("ftruncate de 256 GiB e esparso", fd >= 0 && ftruncate(fd, 1ll << 38) == 0 &&
                                               fstat(fd, &st) == 0 && st.st_size == (1ll << 38) &&
                                               f0 - free_pages() < 8);
    char c = 1;
    check("ler no meio do esparso da zero", fd >= 0 && pread(fd, &c, 1, 1ll << 37) == 1 && c == 0);
    check("escrever no fim do esparso", fd >= 0 && pwrite(fd, "W", 1, (1ll << 38) - 1) == 1);
    check("o fim usa poucas paginas", f0 - free_pages() < 16);
    if (fd >= 0)
        close(fd);
    unlink(nm);

    for (int i = 0; i < 5; i++) {
        snprintf(nm, sizeof(nm), "%s/r%d", m, i);
        fd = open(nm, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }
    DIR *d = opendir(m);
    int seen = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && seen < 50) {
        seen++;
        char a[600], b[600];
        snprintf(a, sizeof(a), "%s/%s", m, e->d_name);
        snprintf(b, sizeof(b), "%s/%s.x", m, e->d_name);
        rename(a, b);
    }
    if (d)
        closedir(d);
    check("rename no mesmo diretorio durante o readdir termina", seen == 5);
    d = opendir(m);
    while (d && (e = readdir(d))) {
        snprintf(nm, sizeof(nm), "%s/%s", m, e->d_name);
        unlink(nm);
    }
    if (d)
        closedir(d);

    char deep[600];
    strcpy(deep, m);
    int depth = 0;
    int dfd = open(m, O_RDONLY | O_DIRECTORY);
    for (; depth < 600 && dfd >= 0; depth++) {
        if (mkdirat(dfd, "d", 0755) < 0)
            break;
        int nfd = openat(dfd, "d", O_RDONLY | O_DIRECTORY);
        close(dfd);
        dfd = nfd;
    }
    if (dfd >= 0)
        close(dfd);
    check("600 diretorios aninhados", depth == 600);
    check("umount com a arvore funda", umount(m) == 0);
    check("rmdir do ponto", rmdir(m) == 0);

    mkdir(m, 0755);
    check("opcao desconhecida da EINVAL", mount("tmpfs", m, "tmpfs", 0, "tamanho=1") < 0 && errno == EINVAL);
    check("size=0 e ilimitado", mount("tmpfs", m, "tmpfs", 0, "size=0") == 0 && umount(m) == 0);
    rmdir(m);
}

int main(int argc, char **argv)
{
    printf("\n");
    strncpy(dir, argc > 1 ? argv[1] : "/tmp", sizeof(dir) - 1);
    if (free_pages() < 0) {
        printf("tmptest: sem sysinfo\n");
        return 1;
    }
    printf("tmptest: 1000 arquivos em %s\n", dir);
    test_thousand();
    printf("tmptest: arquivo grande, buraco e unlink aberto\n");
    test_big();
    printf("tmptest: limite de tamanho\n");
    test_limit();
    printf("tmptest: correcoes da revisao\n");
    test_review();
    printf("tmptest: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
