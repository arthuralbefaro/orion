#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONF "/etc/pkg.conf"
#define DEFAULT_REPO "http://10.0.2.2:8088/repo"
#define STATE "/var/lib/pkg"
#define INSTALLED "/var/lib/pkg/installed"
#define CACHE "/var/cache/pkg"
#define INDEX_FILE "/var/lib/pkg/INDEX"
#define MAX_FILES 256

struct entry {
    char name[64];
    char version[32];
    char file[128];
    unsigned long size;
    char sha[65];
    char desc[160];
};

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++)
        sha256_block(h, data + i * 64);
    uint8_t tail[128];
    size_t rest = len - full * 64;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < blocks; i++)
        sha256_block(h, tail + i * 64);
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", h[i]);
}

static void mkdirs(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void repo_url(char *out, size_t cap)
{
    snprintf(out, cap, "%s", DEFAULT_REPO);
    FILE *f = fopen(CONF, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char url[200];
        if (sscanf(line, "repo %199s", url) == 1)
            snprintf(out, cap, "%s", url);
    }
    fclose(f);
}

static int fetch(const char *url, const char *dest)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execl("/bin/wget", "wget", "-q", "-O", dest, url, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int update(void)
{
    char repo[256], url[320];
    repo_url(repo, sizeof(repo));
    snprintf(url, sizeof(url), "%s/INDEX", repo);
    mkdirs(STATE);
    if (fetch(url, INDEX_FILE) < 0) {
        fprintf(stderr, "pkg: nao consegui baixar %s\n", url);
        return -1;
    }
    FILE *f = fopen(INDEX_FILE, "r");
    int n = 0;
    char line[512];
    while (f && fgets(line, sizeof(line), f))
        if (line[0] && line[0] != '\n')
            n++;
    if (f)
        fclose(f);
    printf("pkg: indice atualizado de %s, %d pacote(s)\n", repo, n);
    return 0;
}

static bool find_entry(const char *name, struct entry *e)
{
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        struct entry t;
        memset(&t, 0, sizeof(t));
        int used = 0;
        if (sscanf(line, "%63s %31s %127s %lu %64s %n", t.name, t.version, t.file, &t.size, t.sha, &used) < 5)
            continue;
        snprintf(t.desc, sizeof(t.desc), "%s", line + used);
        t.desc[strcspn(t.desc, "\n")] = 0;
        if (!strcmp(t.name, name)) {
            *e = t;
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool installed_version(const char *name, char *version, size_t cap)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", INSTALLED, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        char v[64];
        if (sscanf(line, "versao %63s", v) == 1) {
            snprintf(version, cap, "%s", v);
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

static bool safe_path(const char *p)
{
    if (!p[0] || p[0] == '/' || strstr(p, ".."))
        return false;
    return true;
}

static unsigned long octal(const char *s, size_t n)
{
    unsigned long v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7')
            continue;
        v = v * 8 + (unsigned long)(s[i] - '0');
    }
    return v;
}

static int install(const char *name)
{
    struct entry e;
    if (!find_entry(name, &e)) {
        if (update() < 0)
            return 1;
        if (!find_entry(name, &e)) {
            fprintf(stderr, "pkg: %s nao esta no repositorio\n", name);
            return 1;
        }
    }
    char repo[256], url[400], dest[300];
    repo_url(repo, sizeof(repo));
    snprintf(url, sizeof(url), "%s/%s", repo, e.file);
    mkdirs(CACHE);
    snprintf(dest, sizeof(dest), "%s/%s", CACHE, e.file);
    if (fetch(url, dest) < 0) {
        fprintf(stderr, "pkg: nao consegui baixar %s\n", url);
        return 1;
    }
    size_t len = 0;
    uint8_t *tar = slurp(dest, &len);
    if (!tar) {
        fprintf(stderr, "pkg: %s: %s\n", dest, strerror(errno));
        return 1;
    }
    char sum[65];
    sha256_hex(tar, len, sum);
    if (len != e.size || strcmp(sum, e.sha)) {
        fprintf(stderr, "pkg: %s: a soma SHA-256 (ou o tamanho) nao confere com o indice; nada foi instalado\n",
                e.file);
        unlink(dest);
        free(tar);
        return 1;
    }
    if (len < 512 || strncmp((char *)tar, "MANIFEST", 8)) {
        fprintf(stderr, "pkg: %s nao comeca pelo MANIFEST\n", e.file);
        free(tar);
        return 1;
    }
    char files[MAX_FILES][100];
    int nfiles = 0;
    for (size_t off = 0; off + 512 <= len;) {
        const char *h = (const char *)tar + off;
        if (!h[0])
            break;
        char fname[101];
        memcpy(fname, h, 100);
        fname[100] = 0;
        unsigned long size = octal(h + 124, 12);
        unsigned long mode = octal(h + 100, 8);
        char type = h[156];
        size_t data = off + 512;
        if (data + size > len) {
            fprintf(stderr, "pkg: %s esta cortado\n", e.file);
            free(tar);
            return 1;
        }
        if (strcmp(fname, "MANIFEST") && (type == '0' || type == 0)) {
            if (!safe_path(fname) || nfiles >= MAX_FILES) {
                fprintf(stderr, "pkg: caminho recusado no pacote: %s\n", fname);
                free(tar);
                return 1;
            }
            char full[200];
            snprintf(full, sizeof(full), "/%s", fname);
            char *slash = strrchr(full, '/');
            if (slash && slash != full) {
                *slash = 0;
                mkdirs(full);
                *slash = '/';
            }
            char tmp[220];
            snprintf(tmp, sizeof(tmp), "%s.pkg-novo", full);
            int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)(mode & 0777));
            if (fd < 0 || write(fd, tar + data, size) != (ssize_t)size) {
                fprintf(stderr, "pkg: gravar %s falhou: %s\n", full, strerror(errno));
                if (fd >= 0)
                    close(fd);
                free(tar);
                return 1;
            }
            fchmod(fd, (mode_t)(mode & 0777));
            close(fd);
            if (rename(tmp, full) < 0) {
                fprintf(stderr, "pkg: %s: %s\n", full, strerror(errno));
                free(tar);
                return 1;
            }
            snprintf(files[nfiles++], sizeof(files[0]), "%s", fname);
        }
        off = data + ((size + 511) / 512) * 512;
    }
    free(tar);
    mkdirs(INSTALLED);
    char rec[256];
    snprintf(rec, sizeof(rec), "%s/%s", INSTALLED, e.name);
    FILE *f = fopen(rec, "w");
    if (!f) {
        fprintf(stderr, "pkg: %s: %s\n", rec, strerror(errno));
        return 1;
    }
    fprintf(f, "nome %s\nversao %s\n", e.name, e.version);
    for (int i = 0; i < nfiles; i++)
        fprintf(f, "arquivo %s\n", files[i]);
    fclose(f);
    sync();
    printf("pkg: %s %s instalado (%d arquivo(s), %zu bytes, sha256 %.16s...)\n", e.name, e.version, nfiles, len, sum);
    return 0;
}

static int remove_pkg(const char *name)
{
    char rec[256];
    snprintf(rec, sizeof(rec), "%s/%s", INSTALLED, name);
    FILE *f = fopen(rec, "r");
    if (!f) {
        fprintf(stderr, "pkg: %s nao esta instalado\n", name);
        return 1;
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char path[200];
        if (sscanf(line, "arquivo %199s", path) == 1 && safe_path(path)) {
            char full[220];
            snprintf(full, sizeof(full), "/%s", path);
            if (unlink(full) == 0)
                n++;
        }
    }
    fclose(f);
    unlink(rec);
    sync();
    printf("pkg: %s removido (%d arquivo(s))\n", name, n);
    return 0;
}

static int list(bool available)
{
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        if (update() < 0)
            return 1;
        f = fopen(INDEX_FILE, "r");
    }
    if (!f)
        return 1;
    char line[512];
    int shown = 0;
    while (fgets(line, sizeof(line), f)) {
        struct entry t;
        memset(&t, 0, sizeof(t));
        int used = 0;
        if (sscanf(line, "%63s %31s %127s %lu %64s %n", t.name, t.version, t.file, &t.size, t.sha, &used) < 5)
            continue;
        char v[64];
        bool inst = installed_version(t.name, v, sizeof(v));
        if (!available && !inst)
            continue;
        snprintf(t.desc, sizeof(t.desc), "%s", line + used);
        t.desc[strcspn(t.desc, "\n")] = 0;
        printf("%-10s %-8s %-12s %s\n", t.name, t.version, inst ? "instalado" : "-", t.desc);
        shown++;
    }
    fclose(f);
    if (!shown)
        printf("pkg: nenhum pacote %s\n", available ? "no indice" : "instalado");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: pkg update | install NOME... | remove NOME... | list | disponiveis\n");
        return 2;
    }
    if (!strcmp(argv[1], "update"))
        return update() < 0 ? 1 : 0;
    if (!strcmp(argv[1], "list"))
        return list(false);
    if (!strcmp(argv[1], "disponiveis"))
        return list(true);
    if (!strcmp(argv[1], "install") || !strcmp(argv[1], "remove")) {
        if (argc < 3) {
            fprintf(stderr, "pkg: diga qual pacote\n");
            return 2;
        }
        if (getuid() != 0) {
            fprintf(stderr, "pkg: so o root instala ou remove pacotes\n");
            return 1;
        }
        int bad = 0;
        for (int i = 2; i < argc; i++)
            bad |= !strcmp(argv[1], "install") ? install(argv[i]) : remove_pkg(argv[i]);
        return bad;
    }
    fprintf(stderr, "pkg: comando desconhecido %s\n", argv[1]);
    return 2;
}
