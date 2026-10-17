#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static char base[256];
static char source[256];
static int writable;
static uint32_t crc_table[256];

static void check(const char *what, int ok)
{
    if (ok)
        printf("  %-52s ok\n", what);
    else
        printf("  %-52s FALHA (errno %d)\n", what, errno);
    if (!ok)
        fails++;
}

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static const char *P(const char *rel)
{
    static char bufs[4][512];
    static int n;
    char *b = bufs[n++ & 3];
    snprintf(b, 512, "%s/%s", base, rel[0] == '/' ? rel + 1 : rel);
    return b;
}

static int file_crc(const char *path, long *size, uint32_t *crc)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    static unsigned char buf[65536];
    uint32_t c = 0;
    long total = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        c = crc_update(c, buf, (size_t)n);
        total += n;
    }
    close(fd);
    if (n < 0)
        return -1;
    *size = total;
    *crc = c;
    return 0;
}

static int count_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}

static void test_manifest(void)
{
    static char manifest[65536];
    int fd = open(P("MANIFEST"), O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, manifest, sizeof(manifest) - 1) : -1;
    if (fd >= 0)
        close(fd);
    check("le o MANIFEST", n > 0);
    if (n <= 0)
        return;
    manifest[n] = 0;
    int files = 0, bad = 0;
    for (char *line = strtok(manifest, "\n"); line; line = strtok(NULL, "\n")) {
        char a[300], b[300];
        long size;
        unsigned crc;
        if (sscanf(line, "@link %299s %299s", a, b) == 2) {
            char target[300];
            ssize_t len = readlink(P(a), target, sizeof(target) - 1);
            if (len >= 0)
                target[len] = 0;
            struct stat st;
            int ok = len >= 0 && strcmp(target, b) == 0 && lstat(P(a), &st) == 0 && S_ISLNK(st.st_mode);
            printf("  link %-47s %s\n", a, ok ? "ok" : "FALHA");
            if (!ok)
                fails++;
        } else if (sscanf(line, "@count %299s %ld", a, &size) == 2) {
            int c = count_dir(P(a));
            printf("  diretorio %s tem %d entradas%*s %s\n", a, c, 20, "", c == size ? "ok" : "FALHA");
            if (c != size)
                fails++;
        } else if (sscanf(line, "@tail %299s %ld %x", a, &size, &crc) == 3) {
            struct stat st;
            unsigned char tail[4096];
            int tfd = open(P(a), O_RDONLY);
            int ok = tfd >= 0 && fstat(tfd, &st) == 0 && st.st_size == size &&
                     pread(tfd, tail, sizeof(tail), size - (long)sizeof(tail)) == (ssize_t)sizeof(tail) &&
                     crc_update(0, tail, sizeof(tail)) == crc;
            unsigned char mid = 1;
            ok = ok && pread(tfd, &mid, 1, size / 2) == 1 && mid == 0;
            if (tfd >= 0)
                close(tfd);
            printf("  fim de %-44s %s\n", a, ok ? "ok" : "FALHA");
            if (!ok)
                fails++;
        } else if (sscanf(line, "%299s %ld %x", a, &size, &crc) == 3) {
            long got;
            uint32_t c;
            files++;
            if (file_crc(P(a), &got, &c) < 0 || got != size || c != crc) {
                printf("  arquivo %s diferente (tamanho %ld, crc %08x)\n", a, got, c);
                bad++;
            }
        }
    }
    printf("  %d arquivos conferidos pelo crc32\n", files);
    check("todos os arquivos batem com o MANIFEST", files > 200 && bad == 0);
}

static void find_mount(void)
{
    FILE *m = fopen("/proc/mounts", "r");
    if (!m)
        return;
    char line[600], src[256], point[256], type[32], opts[32];
    while (fgets(line, sizeof(line), m)) {
        if (sscanf(line, "%255s %255s %31s %31s", src, point, type, opts) == 4 && strcmp(point, base) == 0) {
            strcpy(source, src);
            writable = strncmp(opts, "rw", 2) == 0;
        }
    }
    fclose(m);
}

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        p[i] = (unsigned char)(seed >> 16);
    }
}

static int all_zero(const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

static void test_semantics(void)
{
    struct stat st, sr;
    check("stat da raiz montada", stat(base, &st) == 0 && S_ISDIR(st.st_mode));
    check("st_dev da montagem difere do de /", stat("/", &sr) == 0 && st.st_dev != sr.st_dev);
    check("dir/sub/deep/.. e dir/sub", stat(P("dir/sub/deep/.."), &st) == 0 && stat(P("dir/sub"), &sr) == 0 &&
                                         st.st_ino == sr.st_ino);
    check("inexistente da ENOENT", stat(P("nao-existe"), &st) < 0 && errno == ENOENT);
    check("arquivo no meio do caminho da ENOTDIR", stat(P("dir/a.txt/x"), &st) < 0 && errno == ENOTDIR);
    if (writable) {
        check("criar arquivo ainda da EPERM", open(P("novo"), O_CREAT | O_WRONLY, 0644) < 0 && errno == EPERM);
        check("mkdir ainda da EPERM", mkdir(P("novo-dir"), 0755) < 0 && errno == EPERM);
    } else {
        check("criar arquivo da EROFS", open(P("novo"), O_CREAT | O_WRONLY, 0644) < 0 && errno == EROFS);
        check("mkdir da EROFS", mkdir(P("novo-dir"), 0755) < 0 && errno == EROFS);
        check("abrir para escrita da EROFS", open(P("dir/a.txt"), O_WRONLY) < 0 && errno == EROFS);
    }
    check("readlink de arquivo comum da EINVAL", readlink(P("dir/a.txt"), (char[8]){0}, 8) < 0 && errno == EINVAL);
    check("arquivo vazio tem tamanho 0", stat(P("empty"), &st) == 0 && st.st_size == 0 && S_ISREG(st.st_mode));

    long size;
    uint32_t crc;
    int fd = open(P("big.bin"), O_RDONLY);
    if (fd >= 0 && fstat(fd, &st) == 0 && file_crc(P("big.bin"), &size, &crc) == 0) {
        unsigned char *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        check("mmap do big.bin bate com o read", m != MAP_FAILED && crc_update(0, m, (size_t)st.st_size) == crc);
        if (m != MAP_FAILED)
            munmap(m, (size_t)st.st_size);
    } else {
        check("abre o big.bin", 0);
    }
    if (fd >= 0)
        close(fd);
}

static void test_write(void)
{
    static unsigned char a[300 * 1024 + 7], back[sizeof(a)];
    struct stat st;
    long far = 20L * 1024 * 1024;
    fill(a, sizeof(a), 1);

    int fd = open(P("empty"), O_RDWR);
    check("abre o empty para escrita", fd >= 0);
    if (fd < 0)
        return;
    check("escreve 300 KiB (blocos indiretos)", write(fd, a, sizeof(a)) == (ssize_t)sizeof(a));
    check("le de volta igual", pread(fd, back, sizeof(back), 0) == (ssize_t)sizeof(back) &&
                                   memcmp(a, back, sizeof(a)) == 0);
    check("pwrite em 20 MiB (arquivo esparso)", pwrite(fd, "fim", 3, far) == 3);
    check("tamanho cresceu para 20 MiB + 3", fstat(fd, &st) == 0 && st.st_size == far + 3);
    check("buraco no meio le zeros", pread(fd, back, 4096, 5L * 1024 * 1024) == 4096 && all_zero(back, 4096));
    unsigned char *m = mmap(NULL, sizeof(a), PROT_READ, MAP_PRIVATE, fd, 0);
    check("mmap ve o que o write gravou", m != MAP_FAILED && memcmp(m, a, sizeof(a)) == 0);
    if (m != MAP_FAILED)
        munmap(m, sizeof(a));
    check("ftruncate para 100000", ftruncate(fd, 100000) == 0 && fstat(fd, &st) == 0 && st.st_size == 100000);
    check("ftruncate para 200000 cresce com zeros", ftruncate(fd, 200000) == 0 &&
                                                    pread(fd, back, 100000, 100000) == 100000 &&
                                                    all_zero(back, 100000));
    check("ftruncate para 0 devolve os blocos", ftruncate(fd, 0) == 0 && fstat(fd, &st) == 0 &&
                                                st.st_size == 0 && st.st_blocks == 0);
    check("escreve o conteudo que deve persistir", pwrite(fd, "persiste\n", 9, 0) == 9);
    close(fd);

    if (!source[0]) {
        check("acha a origem da montagem em /proc/mounts", 0);
        return;
    }
    check("desmonta", umount(base) == 0);
    check("monta de novo", mount(source, base, "ext2", 0, NULL) == 0);
    char got[16] = { 0 };
    fd = open(P("empty"), O_RDWR);
    check("conteudo sobreviveu a desmontagem", fd >= 0 && read(fd, got, sizeof(got)) == 9 &&
                                               memcmp(got, "persiste\n", 9) == 0);
    check("volta o empty ao tamanho 0", fd >= 0 && ftruncate(fd, 0) == 0);
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv)
{
    printf("\n");
    if (argc < 2) {
        fprintf(stderr, "uso: ext2test <ponto de montagem>\n");
        return 2;
    }
    crc_init();
    strncpy(base, argv[1], sizeof(base) - 1);
    find_mount();
    printf("ext2test: conferindo %s (%s)\n", base, writable ? "leitura e escrita" : "somente leitura");
    test_manifest();
    test_semantics();
    if (writable)
        test_write();
    printf("ext2test: %s (%d falha(s))\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
