#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SECTOR        2048
#define SYSTEM_AREA   16
#define MAX_RR_NAME   150
#define MAX_DEPTH     8
#define ISO_NAME_MAX  31

struct node {
    char *name;
    char iso[ISO_NAME_MAX + 3];
    char *host;
    int is_dir;
    uint64_t size;
    uint32_t lba;
    uint32_t mode;
    uint32_t number;
    struct node *parent;
    struct node **kids;
    int nkids;
};

struct image {
    uint8_t *data;
    uint64_t size;
};

static struct node **dirs;
static int ndirs;
static time_t build_time;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("mkiso: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p)
        die("sem memoria");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xcalloc(strlen(s) + 1, 1);
    strcpy(p, s);
    return p;
}

static char *join(const char *a, const char *b)
{
    char *p = xcalloc(strlen(a) + strlen(b) + 2, 1);
    sprintf(p, "%s/%s", a, b);
    return p;
}

static void put16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32le(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32be(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (24 - 8 * i));
}

static void put16both(uint8_t *p, uint16_t v)
{
    put16le(p, v);
    put16be(p + 2, v);
}

static void put32both(uint8_t *p, uint32_t v)
{
    put32le(p, v);
    put32be(p + 4, v);
}

static void put_padded(uint8_t *p, const char *s, size_t len)
{
    memset(p, ' ', len);
    size_t n = strlen(s);
    memcpy(p, s, n < len ? n : len);
}

static int iso_char(int c)
{
    c = toupper((unsigned char)c);
    return (isupper(c) || isdigit(c) || c == '_') ? c : '_';
}

static void make_iso_name(struct node *n)
{
    char base[ISO_NAME_MAX + 1] = { 0 }, ext[8] = { 0 };
    const char *dot = n->is_dir ? NULL : strrchr(n->name, '.');
    size_t blen = dot ? (size_t)(dot - n->name) : strlen(n->name);
    size_t bmax = n->is_dir ? ISO_NAME_MAX : 24;
    for (size_t i = 0; i < blen && i < bmax; i++)
        base[i] = (char)iso_char(n->name[i]);
    if (!base[0])
        base[0] = '_';
    if (dot)
        for (size_t i = 0; dot[1 + i] && i < 5; i++)
            ext[i] = (char)iso_char(dot[1 + i]);
    if (n->is_dir)
        snprintf(n->iso, sizeof(n->iso), "%s", base);
    else
        snprintf(n->iso, sizeof(n->iso), "%s.%s;1", base, ext);
}

static void uniquify(struct node *dir)
{
    for (int i = 0; i < dir->nkids; i++) {
        for (int tries = 1; tries < 1000; tries++) {
            int clash = 0;
            for (int j = 0; j < i; j++)
                if (strcmp(dir->kids[i]->iso, dir->kids[j]->iso) == 0)
                    clash = 1;
            if (!clash)
                break;
            char suffix[8];
            snprintf(suffix, sizeof(suffix), "~%d", tries);
            char *iso = dir->kids[i]->iso;
            char *semi = strchr(iso, ';');
            char *dot = strchr(iso, '.');
            char *cut = dot ? dot : (semi ? semi : iso + strlen(iso));
            char tail[16];
            snprintf(tail, sizeof(tail), "%s", cut);
            size_t keep = (size_t)(cut - iso);
            if (keep + strlen(suffix) > 24)
                keep = 24 - strlen(suffix);
            snprintf(iso + keep, sizeof(dir->kids[i]->iso) - keep, "%s%s", suffix, tail);
        }
    }
}

static int by_iso(const void *a, const void *b)
{
    const struct node *x = *(const struct node *const *)a, *y = *(const struct node *const *)b;
    return strcmp(x->iso, y->iso);
}

static struct node *scan(const char *host, const char *name, struct node *parent, int depth)
{
    struct stat st;
    if (stat(host, &st) != 0)
        die("nao achei %s", host);
    struct node *n = xcalloc(1, sizeof(*n));
    n->name = xstrdup(name);
    n->host = xstrdup(host);
    n->parent = parent;
    n->is_dir = S_ISDIR(st.st_mode);
    if (!n->is_dir) {
        n->size = (uint64_t)st.st_size;
        n->mode = 0100644;
        if (n->size >= 0xFFFFFFFFull)
            die("%s e grande demais para ISO9660", host);
        make_iso_name(n);
        return n;
    }
    n->mode = 040755;
    if (parent)
        make_iso_name(n);
    if (depth >= MAX_DEPTH)
        die("%s passa de %d niveis", host, MAX_DEPTH);
    DIR *d = opendir(host);
    if (!d)
        die("nao abri o diretorio %s", host);
    struct dirent *e;
    int cap = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (strlen(e->d_name) > MAX_RR_NAME)
            die("nome longo demais: %s", e->d_name);
        if (n->nkids == cap) {
            cap = cap ? cap * 2 : 16;
            struct node **k = realloc(n->kids, (size_t)cap * sizeof(*k));
            if (!k)
                die("sem memoria");
            n->kids = k;
        }
        char *child = join(host, e->d_name);
        n->kids[n->nkids++] = scan(child, e->d_name, n, depth + 1);
        free(child);
    }
    closedir(d);
    qsort(n->kids, (size_t)n->nkids, sizeof(*n->kids), by_iso);
    uniquify(n);
    qsort(n->kids, (size_t)n->nkids, sizeof(*n->kids), by_iso);
    return n;
}

static void collect_dirs(struct node *root)
{
    dirs = xcalloc(4096, sizeof(*dirs));
    dirs[ndirs++] = root;
    for (int i = 0; i < ndirs; i++)
        for (int k = 0; k < dirs[i]->nkids; k++)
            if (dirs[i]->kids[k]->is_dir) {
                if (ndirs == 4096)
                    die("diretorios demais");
                dirs[ndirs++] = dirs[i]->kids[k];
            }
    for (int i = 0; i < ndirs; i++)
        dirs[i]->number = (uint32_t)(i + 1);
}

static size_t su_len(const struct node *n, int kind)
{
    size_t len = 36;
    if (kind == 1 && !n->parent)
        len += 7 + 8 + 10 + 5 + 5;
    if (kind == 0)
        len += 5 + strlen(n->name);
    return len;
}

static size_t record_len(const struct node *n, int kind)
{
    size_t id = kind ? 1 : strlen(n->iso);
    size_t len = 33 + id + ((id & 1) ? 0 : 1);
    len += su_len(n, kind);
    if (len & 1)
        len++;
    if (len > 255)
        die("registro grande demais para %s", n->name);
    return len;
}

static uint64_t dir_extent_size(const struct node *d)
{
    uint64_t off = 0;
    for (int i = -2; i < d->nkids; i++) {
        size_t len = i < 0 ? record_len(d, i == -2 ? 1 : 2) : record_len(d->kids[i], 0);
        if (off % SECTOR + len > SECTOR)
            off += SECTOR - off % SECTOR;
        off += len;
    }
    return (off + SECTOR - 1) / SECTOR * SECTOR;
}

static void put_time7(uint8_t *p)
{
    struct tm *t = gmtime(&build_time);
    p[0] = (uint8_t)t->tm_year;
    p[1] = (uint8_t)(t->tm_mon + 1);
    p[2] = (uint8_t)t->tm_mday;
    p[3] = (uint8_t)t->tm_hour;
    p[4] = (uint8_t)t->tm_min;
    p[5] = (uint8_t)t->tm_sec;
    p[6] = 0;
}

static void put_time17(uint8_t *p)
{
    struct tm *t = gmtime(&build_time);
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d00", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    memcpy(p, buf, 16);
    p[16] = 0;
}

static size_t write_record(uint8_t *p, const struct node *n, int kind)
{
    const struct node *target = kind == 2 ? (n->parent ? n->parent : n) : n;
    size_t len = record_len(n, kind);
    memset(p, 0, len);
    size_t id = kind ? 1 : strlen(n->iso);
    p[0] = (uint8_t)len;
    put32both(p + 2, target->lba);
    put32both(p + 10, (uint32_t)target->size);
    put_time7(p + 18);
    p[25] = target->is_dir ? 2 : 0;
    put16both(p + 28, 1);
    p[32] = (uint8_t)id;
    if (kind == 1)
        p[33] = 0;
    else if (kind == 2)
        p[33] = 1;
    else
        memcpy(p + 33, n->iso, id);
    uint8_t *su = p + 33 + id + ((id & 1) ? 0 : 1);
    if (kind == 1 && !n->parent) {
        memcpy(su, "SP", 2);
        su[2] = 7;
        su[3] = 1;
        su[4] = 0xBE;
        su[5] = 0xEF;
        su[6] = 0;
        su += 7;
        memcpy(su, "ER", 2);
        su[2] = 8 + 10 + 5 + 5;
        su[3] = 1;
        su[4] = 10;
        su[5] = 5;
        su[6] = 5;
        su[7] = 1;
        memcpy(su + 8, "RRIP_1991A", 10);
        memcpy(su + 18, "ORION", 5);
        memcpy(su + 23, "ORION", 5);
        su += 28;
    }
    memcpy(su, "PX", 2);
    su[2] = 36;
    su[3] = 1;
    put32both(su + 4, target->mode);
    put32both(su + 12, target->is_dir ? 2 : 1);
    put32both(su + 20, 0);
    put32both(su + 28, 0);
    su += 36;
    if (kind == 0) {
        size_t nl = strlen(n->name);
        memcpy(su, "NM", 2);
        su[2] = (uint8_t)(5 + nl);
        su[3] = 1;
        su[4] = 0;
        memcpy(su + 5, n->name, nl);
    }
    return len;
}

static void write_dir(uint8_t *img, const struct node *d)
{
    uint8_t *base = img + (uint64_t)d->lba * SECTOR;
    uint64_t off = 0;
    for (int i = -2; i < d->nkids; i++) {
        const struct node *n = i < 0 ? d : d->kids[i];
        int kind = i == -2 ? 1 : (i == -1 ? 2 : 0);
        size_t len = record_len(n, kind);
        if (off % SECTOR + len > SECTOR)
            off += SECTOR - off % SECTOR;
        write_record(base + off, n, kind);
        off += len;
    }
}

static size_t path_table(uint8_t *p, int big_endian)
{
    size_t off = 0;
    for (int i = 0; i < ndirs; i++) {
        const struct node *d = dirs[i];
        size_t id = d->parent ? strlen(d->iso) : 1;
        if (p) {
            p[off] = (uint8_t)id;
            p[off + 1] = 0;
            if (big_endian) {
                put32be(p + off + 2, d->lba);
                put16be(p + off + 6, (uint16_t)(d->parent ? d->parent->number : 1));
            } else {
                put32le(p + off + 2, d->lba);
                put16le(p + off + 6, (uint16_t)(d->parent ? d->parent->number : 1));
            }
            if (d->parent)
                memcpy(p + off + 8, d->iso, id);
            else
                p[off + 8] = 0;
        }
        off += 8 + id + (id & 1);
    }
    return off;
}

static struct node *find(struct node *root, const char *path)
{
    struct node *cur = root;
    char *copy = xstrdup(path);
    for (char *part = strtok(copy, "/"); part && cur; part = strtok(NULL, "/")) {
        struct node *next = NULL;
        for (int i = 0; i < cur->nkids; i++)
            if (strcmp(cur->kids[i]->name, part) == 0)
                next = cur->kids[i];
        cur = next;
    }
    free(copy);
    return cur;
}

static void assign_files(struct node *n, uint32_t *next)
{
    for (int i = 0; i < n->nkids; i++) {
        struct node *k = n->kids[i];
        if (k->is_dir) {
            assign_files(k, next);
        } else {
            k->lba = *next;
            *next += (uint32_t)((k->size + SECTOR - 1) / SECTOR);
            if (!k->size)
                k->lba = 0;
        }
    }
}

static void copy_files(uint8_t *img, struct node *n)
{
    for (int i = 0; i < n->nkids; i++) {
        struct node *k = n->kids[i];
        if (k->is_dir) {
            copy_files(img, k);
            continue;
        }
        if (!k->size)
            continue;
        FILE *f = fopen(k->host, "rb");
        if (!f || fread(img + (uint64_t)k->lba * SECTOR, 1, k->size, f) != k->size)
            die("nao li %s", k->host);
        fclose(f);
    }
}

static void boot_info_table(uint8_t *img, const struct node *bios)
{
    uint8_t *file = img + (uint64_t)bios->lba * SECTOR;
    if (bios->size < 64)
        die("imagem de boot BIOS pequena demais");
    uint32_t sum = 0;
    for (uint64_t off = 64; off + 4 <= bios->size; off += 4)
        sum += (uint32_t)file[off] | ((uint32_t)file[off + 1] << 8) | ((uint32_t)file[off + 2] << 16) |
               ((uint32_t)file[off + 3] << 24);
    put32le(file + 8, SYSTEM_AREA);
    put32le(file + 12, bios->lba);
    put32le(file + 16, (uint32_t)bios->size);
    put32le(file + 20, sum);
    memset(file + 24, 0, 40);
}

static void boot_catalog(uint8_t *p, const struct node *bios, const struct node *efi)
{
    memset(p, 0, SECTOR);
    p[0] = 1;
    p[1] = 0;
    memcpy(p + 4, "ORION", 5);
    p[30] = 0x55;
    p[31] = 0xAA;
    uint16_t sum = 0;
    for (int i = 0; i < 32; i += 2)
        sum = (uint16_t)(sum + (p[i] | (p[i + 1] << 8)));
    put16le(p + 28, (uint16_t)(0x10000 - sum));
    uint8_t *e = p + 32;
    e[0] = 0x88;
    e[1] = 0;
    put16le(e + 6, 4);
    put32le(e + 8, bios->lba);
    uint8_t *h = p + 64;
    h[0] = 0x91;
    h[1] = 0xEF;
    put16le(h + 2, 1);
    uint8_t *s = p + 96;
    s[0] = 0x88;
    s[1] = 0;
    uint64_t count = (efi->size + 511) / 512;
    put16le(s + 6, (uint16_t)(count > 0xFFFF ? 0 : count));
    put32le(s + 8, efi->lba);
}

static void hybrid_mbr(uint8_t *img, const struct node *efi)
{
    uint8_t *mbr = img;
    uint32_t sig = (uint32_t)build_time ^ 0x4F52494Fu;
    put32le(mbr + 440, sig);
    uint8_t *e = mbr + 446;
    e[0] = 0x80;
    e[1] = 0xFE;
    e[2] = 0xFF;
    e[3] = 0xFF;
    e[4] = 0xEF;
    e[5] = 0xFE;
    e[6] = 0xFF;
    e[7] = 0xFF;
    put32le(e + 8, efi->lba * (SECTOR / 512));
    put32le(e + 12, (uint32_t)((efi->size + 511) / 512));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

static void volume_descriptors(uint8_t *img, const struct node *root, uint32_t total, uint32_t pt_size,
                               uint32_t l_lba, uint32_t m_lba, uint32_t cat_lba, const char *label)
{
    uint8_t *pvd = img + SYSTEM_AREA * SECTOR;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    put_padded(pvd + 8, "ORION", 32);
    char vol[33] = { 0 };
    for (size_t i = 0; label[i] && i < 32; i++)
        vol[i] = (char)iso_char(label[i]);
    put_padded(pvd + 40, vol, 32);
    put32both(pvd + 80, total);
    put16both(pvd + 120, 1);
    put16both(pvd + 124, 1);
    put16both(pvd + 128, SECTOR);
    put32both(pvd + 132, pt_size);
    put32le(pvd + 140, l_lba);
    put32be(pvd + 148, m_lba);
    write_record(pvd + 156, root, 1);
    pvd[156] = 34;
    put_padded(pvd + 190, "", 128);
    put_padded(pvd + 318, "ORION", 128);
    put_padded(pvd + 446, "ORION MKISO", 128);
    put_padded(pvd + 574, "ORION", 128);
    put_padded(pvd + 702, "", 37);
    put_padded(pvd + 739, "", 37);
    put_padded(pvd + 776, "", 37);
    put_time17(pvd + 813);
    put_time17(pvd + 830);
    memset(pvd + 847, '0', 16);
    put_time17(pvd + 864);
    pvd[881] = 1;

    uint8_t *brvd = pvd + SECTOR;
    brvd[0] = 0;
    memcpy(brvd + 1, "CD001", 5);
    brvd[6] = 1;
    memcpy(brvd + 7, "EL TORITO SPECIFICATION", 23);
    put32le(brvd + 0x47, cat_lba);

    uint8_t *term = brvd + SECTOR;
    term[0] = 255;
    memcpy(term + 1, "CD001", 5);
    term[6] = 1;
}

int main(int argc, char **argv)
{
    const char *bios_path = NULL, *efi_path = NULL, *label = "ORION";
    const char *src = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
            bios_path = argv[++i];
        else if (strcmp(argv[i], "--efi") == 0 && i + 1 < argc)
            efi_path = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
            label = argv[++i];
        else if (!src)
            src = argv[i];
        else if (!out)
            out = argv[i];
        else
            die("argumento a mais: %s", argv[i]);
    }
    if (!src || !out || !bios_path || !efi_path)
        die("uso: mkiso <diretorio> <saida.iso> --bios <caminho> --efi <caminho> [--label NOME]");

    build_time = time(NULL);
    struct node *root = scan(src, "", NULL, 0);
    collect_dirs(root);
    struct node *bios = find(root, bios_path), *efi = find(root, efi_path);
    if (!bios || bios->is_dir)
        die("imagem BIOS %s nao esta na arvore", bios_path);
    if (!efi || efi->is_dir)
        die("imagem EFI %s nao esta na arvore", efi_path);

    uint32_t cat_lba = SYSTEM_AREA + 3;
    uint32_t pt_size = (uint32_t)path_table(NULL, 0);
    uint32_t pt_sectors = (pt_size + SECTOR - 1) / SECTOR;
    uint32_t l_lba = cat_lba + 1, m_lba = l_lba + pt_sectors;
    uint32_t next = m_lba + pt_sectors;
    for (int i = 0; i < ndirs; i++) {
        dirs[i]->size = dir_extent_size(dirs[i]);
        dirs[i]->lba = next;
        next += (uint32_t)(dirs[i]->size / SECTOR);
    }
    assign_files(root, &next);
    uint32_t total = next;
    if (total % 32)
        total += 32 - total % 32;

    uint64_t bytes = (uint64_t)total * SECTOR;
    uint8_t *img = xcalloc(1, (size_t)bytes);
    for (int i = 0; i < ndirs; i++)
        write_dir(img, dirs[i]);
    path_table(img + (uint64_t)l_lba * SECTOR, 0);
    path_table(img + (uint64_t)m_lba * SECTOR, 1);
    copy_files(img, root);
    boot_info_table(img, bios);
    boot_catalog(img + (uint64_t)cat_lba * SECTOR, bios, efi);
    volume_descriptors(img, root, total, pt_size, l_lba, m_lba, cat_lba, label);
    hybrid_mbr(img, efi);

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(img, 1, (size_t)bytes, f) != bytes || fclose(f) != 0)
        die("nao gravei %s", out);
    printf("mkiso: %s, %u setores de 2048 bytes (%.1f MiB), %d diretorio(s); BIOS em %u, EFI em %u\n", out,
           total, (double)bytes / (1024.0 * 1024.0), ndirs, bios->lba, efi->lba);
    free(img);
    return 0;
}
