#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static __thread int tls_counter = 5;
static int fails;

static void check(const char *what, int ok)
{
    printf("  %-44s %s\n", what, ok ? "ok" : "FALHA");
    if (!ok)
        fails++;
}

static int cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(int argc, char **argv)
{
    printf("mhello: programa compilado contra a musl (argc=%d, argv[0]=%s)\n", argc, argv[0]);

    tls_counter += 2;
    check("variavel __thread (TLS)", tls_counter == 7);

    const char *path = getenv("PATH");
    check("getenv(PATH)", path && strcmp(path, "/bin") == 0);

    errno = 0;
    int fd = open("/nao/existe", O_RDONLY);
    check("errno = ENOENT num open que falha", fd < 0 && errno == ENOENT);
    check("strerror", strcmp(strerror(ENOENT), "No such file or directory") == 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f|%5.1e|%g", 3.375, 12345.678, 0.5);
    check("printf com ponto flutuante", strcmp(buf, "3.375|1.2e+04|0.5") == 0);

    char *small = malloc(64);
    char *big = malloc(1 << 20);
    check("malloc pequeno e grande", small && big);
    memset(big, 7, 1 << 20);
    check("memoria do malloc grande", big[(1 << 20) - 1] == 7);
    free(small);
    free(big);

    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof(int), cmp);
    check("qsort", v[0] == 1 && v[4] == 9);

    struct stat st;
    check("stat de /bin/sh", stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);

    DIR *d = opendir("/bin");
    int n = 0, has_sh = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        n++;
        if (strcmp(e->d_name, "sh") == 0)
            has_sh = 1;
    }
    if (d)
        closedir(d);
    check("opendir/readdir de /bin", n > 20 && has_sh);

    struct utsname u;
    check("uname", uname(&u) == 0 && strcmp(u.sysname, "Orion") == 0);

    time_t now = time(NULL);
    check("time() depois de 2024", now > 1704067200);
    check("getpid", getpid() > 1);

    size_t big_len = 4u << 20;
    unsigned char *r = malloc(big_len);
    for (size_t i = 0; r && i < big_len; i += 4096)
        r[i] = (unsigned char)(i >> 12);
    unsigned char *r2 = r ? realloc(r, 4 * big_len) : NULL;
    int kept = r2 != NULL;
    for (size_t i = 0; kept && i < big_len; i += 4096)
        if (r2[i] != (unsigned char)(i >> 12))
            kept = 0;
    if (r2)
        r2[4 * big_len - 1] = 1;
    free(r2);
    check("realloc grande preserva o conteudo (mremap)", kept);

    size_t pg = 4096;
    unsigned char *m = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *blocker = mmap(m + 4 * pg, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    for (int i = 0; i < 4; i++)
        m[i * pg] = (unsigned char)(0x40 + i);
    unsigned char *moved = mremap(m, 4 * pg, 16 * pg, MREMAP_MAYMOVE);
    int ok_move = moved != MAP_FAILED && moved != m && blocker == m + 4 * pg;
    for (int i = 0; ok_move && i < 4; i++)
        if (moved[i * pg] != (unsigned char)(0x40 + i))
            ok_move = 0;
    if (ok_move)
        moved[15 * pg] = 9;
    check("mremap move a area e leva as paginas", ok_move);
    unsigned char *shrunk = ok_move ? mremap(moved, 16 * pg, 2 * pg, 0) : MAP_FAILED;
    check("mremap encolhe no lugar", shrunk == moved && shrunk[pg] == 0x41);
    unsigned char *grown = shrunk != MAP_FAILED ? mremap(shrunk, 2 * pg, 3 * pg, 0) : MAP_FAILED;
    check("mremap cresce no lugar sem MAYMOVE", grown == shrunk && grown[2 * pg] == 0);
    errno = 0;
    void *bad = mremap(m == moved ? NULL : m, pg, 2 * pg, MREMAP_MAYMOVE);
    check("mremap de area desfeita da EFAULT", bad == MAP_FAILED && errno == EFAULT);

    printf("mhello: fim, %s\n", fails ? "FALHOU" : "TUDO OK");
    return fails ? 1 : 0;
}
