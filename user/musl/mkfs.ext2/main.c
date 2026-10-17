#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <orion.h>

#define INODE_SIZE 128
#define ROOT_INO   2
#define FIRST_INO  11
#define LPF_BYTES  16384
#define MIN_DATA   50

struct group {
    uint32_t start, end, bb, ib, it, data;
    int super;
};

static const char *dev;
static int fd;
static uint32_t bs, blocks, first_data, bpg, groups, ipg, itb, gdt_blocks;
static struct group *gr;

static void put16(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[off + i] = (uint8_t)(v >> (8 * i));
}

static int die(const char *what)
{
    fprintf(stderr, "mkfs.ext2: %s: %s\n", dev, what);
    return 1;
}

static int has_super(uint32_t g)
{
    if (g <= 1)
        return 1;
    static const uint32_t bases[] = { 3, 5, 7 };
    for (int i = 0; i < 3; i++) {
        uint64_t n = bases[i];
        while (n < g)
            n *= bases[i];
        if (n == g)
            return 1;
    }
    return 0;
}

static int put(uint64_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
        return -1;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int zero(uint64_t off, uint64_t len)
{
    static uint8_t z[65536];
    while (len) {
        size_t n = len < sizeof(z) ? (size_t)len : sizeof(z);
        if (put(off, z, n) < 0)
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

static uint32_t overhead(uint32_t g)
{
    return (has_super(g) ? 1 + gdt_blocks : 0) + 2 + itb;
}

static int mounted(void)
{
    FILE *m = fopen("/proc/mounts", "r");
    if (!m)
        return 0;
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    char line[600], src[256];
    int busy = 0;
    while (!busy && fgets(line, sizeof(line), m)) {
        if (sscanf(line, "%255s", src) != 1)
            continue;
        const char *sb = strrchr(src, '/');
        int devlike = !sb || strncmp(src, "/dev/", 5) == 0;
        sb = sb ? sb + 1 : src;
        busy = strcmp(src, dev) == 0 || (devlike && strcmp(sb, base) == 0);
    }
    fclose(m);
    return busy;
}

static void dirent(uint8_t *b, uint32_t pos, uint32_t ino, uint32_t rec, const char *name, uint8_t type)
{
    size_t len = strlen(name);
    put32(b, pos, ino);
    put16(b, pos + 4, rec);
    b[pos + 6] = (uint8_t)len;
    b[pos + 7] = type;
    memcpy(b + pos + 8, name, len);
}

static void inode(uint8_t *raw, uint32_t mode, uint32_t links, uint32_t size, const uint32_t *blk, uint32_t n,
                  uint32_t now)
{
    memset(raw, 0, INODE_SIZE);
    put16(raw, 0, mode);
    put32(raw, 4, size);
    put32(raw, 8, now);
    put32(raw, 12, now);
    put32(raw, 16, now);
    put16(raw, 26, links);
    put32(raw, 28, n * (bs / 512));
    for (uint32_t i = 0; i < n; i++)
        put32(raw, 40 + 4 * i, blk[i]);
}

static void set_bit(uint8_t *map, uint32_t i)
{
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}

static int usage(void)
{
    fprintf(stderr, "uso: mkfs.ext2 [-b 1024|2048|4096] [-L rotulo] [-i bytes-por-inode] [-N inodes] <dispositivo>\n");
    return 1;
}

int main(int argc, char **argv)
{
    uint32_t want_bs = 0, ratio = 0, want_inodes = 0;
    const char *label = "";
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (i + 1 >= argc)
            return usage();
        if (strcmp(argv[i], "-b") == 0)
            want_bs = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-L") == 0)
            label = argv[++i];
        else if (strcmp(argv[i], "-i") == 0)
            ratio = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "-N") == 0)
            want_inodes = (uint32_t)strtoul(argv[++i], NULL, 10);
        else
            return usage();
    }
    if (argc - i != 1)
        return usage();
    dev = argv[i];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (want_bs && want_bs != 1024 && want_bs != 2048 && want_bs != 4096)
        return usage();
    if (strlen(label) > 16)
        return die("rotulo com mais de 16 caracteres");
    if (mounted())
        return die("esta montado; desmonte antes de formatar");

    fd = open(dev, O_RDWR);
    if (fd < 0)
        return die(orion_strerror(errno));
    struct stat st;
    uint64_t size = 0;
    if (fstat(fd, &st) == 0)
        size = (uint64_t)st.st_size;
    if (!size) {
        off_t end = lseek(fd, 0, SEEK_END);
        size = end > 0 ? (uint64_t)end : 0;
    }
    if (size < 1024 * 1024)
        return die("menor que 1 MiB");

    int small = size < 512ull * 1024 * 1024;
    bs = want_bs ? want_bs : (small ? 1024 : 4096);
    if (!ratio)
        ratio = small ? 4096 : 16384;
    if (ratio < bs / 4)
        ratio = bs / 4;
    uint64_t nblocks = size / bs;
    if (nblocks > 0xFFFFFFFFull)
        nblocks = 0xFFFFFFFFull;
    blocks = (uint32_t)nblocks;
    first_data = bs == 1024 ? 1 : 0;
    bpg = 8 * bs;
    groups = (blocks - first_data + bpg - 1) / bpg;

    uint64_t inodes = want_inodes ? want_inodes : (uint64_t)blocks * bs / ratio;
    uint32_t per_block = bs / INODE_SIZE;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t per = (inodes + groups - 1) / groups;
        per = (per + per_block - 1) / per_block * per_block;
        if (per < per_block)
            per = per_block;
        if (per > 8ull * bs)
            per = 8ull * bs;
        while ((uint64_t)per * groups > 0xFFFFFFFFull)
            per -= per_block;
        ipg = (uint32_t)per;
        itb = ipg * INODE_SIZE / bs;
        gdt_blocks = (groups * 32 + bs - 1) / bs;
        uint32_t last = blocks - first_data - (groups - 1) * bpg;
        if (pass == 0 && groups > 1 && last < overhead(groups - 1) + MIN_DATA) {
            blocks -= last;
            groups--;
        }
    }
    if ((uint64_t)ipg * groups < FIRST_INO + 1)
        return die("inodes de menos");

    gr = calloc(groups, sizeof(*gr));
    if (!gr)
        return die("sem memoria");
    for (uint32_t g = 0; g < groups; g++) {
        struct group *p = &gr[g];
        p->start = first_data + g * bpg;
        p->end = g + 1 == groups ? blocks : p->start + bpg;
        p->super = has_super(g);
        uint32_t cur = p->start + (p->super ? 1 + gdt_blocks : 0);
        p->bb = cur++;
        p->ib = cur++;
        p->it = cur;
        p->data = cur + itb;
        if (p->data > p->end)
            return die("pequeno demais para este tamanho de bloco");
    }
    uint32_t lpf_n = LPF_BYTES / bs;
    if (lpf_n > 12)
        lpf_n = 12;
    if (gr[0].data + 1 + lpf_n > gr[0].end)
        return die("pequeno demais");

    uint32_t now = (uint32_t)time(NULL);
    uint32_t root_blk = gr[0].data;
    uint32_t lpf_blk[12];
    for (uint32_t k = 0; k < lpf_n; k++)
        lpf_blk[k] = root_blk + 1 + k;
    uint32_t used0 = 1 + lpf_n;

    printf("mkfs.ext2: %s: %u blocos de %u bytes, %u grupo(s), %u inodes (%u por grupo)\n", dev, blocks, bs, groups,
           ipg * groups, ipg);

    uint8_t *map = malloc(bs);
    uint8_t *gdt = calloc(gdt_blocks, bs);
    uint8_t *blk = malloc(bs);
    if (!map || !gdt || !blk)
        return die("sem memoria");

    uint32_t free_blocks = 0, free_inodes = 0;
    if (zero(0, 1024) < 0)
        return die("erro de escrita");
    for (uint32_t g = 0; g < groups; g++) {
        struct group *p = &gr[g];
        uint32_t len = p->end - p->start;
        memset(map, 0, bs);
        for (uint32_t b = p->start; b < p->data; b++)
            set_bit(map, b - p->start);
        uint32_t fb = len - (p->data - p->start);
        if (g == 0) {
            for (uint32_t k = 0; k < used0; k++)
                set_bit(map, p->data - p->start + k);
            fb -= used0;
        }
        for (uint32_t b = len; b < 8 * bs; b++)
            set_bit(map, b);
        if (put((uint64_t)p->bb * bs, map, bs) < 0)
            return die("erro de escrita no bitmap de blocos");

        memset(map, 0, bs);
        uint32_t fi = ipg;
        if (g == 0) {
            for (uint32_t k = 0; k < FIRST_INO; k++)
                set_bit(map, k);
            fi -= FIRST_INO;
        }
        for (uint32_t k = ipg; k < 8 * bs; k++)
            set_bit(map, k);
        if (put((uint64_t)p->ib * bs, map, bs) < 0)
            return die("erro de escrita no bitmap de inodes");
        if (zero((uint64_t)p->it * bs, (uint64_t)itb * bs) < 0)
            return die("erro de escrita na tabela de inodes");

        put32(gdt, g * 32, p->bb);
        put32(gdt, g * 32 + 4, p->ib);
        put32(gdt, g * 32 + 8, p->it);
        put16(gdt, g * 32 + 12, fb);
        put16(gdt, g * 32 + 14, fi);
        put16(gdt, g * 32 + 16, g == 0 ? 2 : 0);
        free_blocks += fb;
        free_inodes += fi;
        if (g % 8 == 7 || g + 1 == groups)
            printf("mkfs.ext2: %u de %u grupo(s) gravado(s)\n", g + 1, groups);
    }

    uint8_t sb[1024];
    memset(sb, 0, sizeof(sb));
    put32(sb, 0, ipg * groups);
    put32(sb, 4, blocks);
    put32(sb, 12, free_blocks);
    put32(sb, 16, free_inodes);
    put32(sb, 20, first_data);
    uint32_t log = bs == 1024 ? 0 : bs == 2048 ? 1 : 2;
    put32(sb, 24, log);
    put32(sb, 28, log);
    put32(sb, 32, bpg);
    put32(sb, 36, bpg);
    put32(sb, 40, ipg);
    put32(sb, 48, now);
    put16(sb, 54, 0xFFFF);
    put16(sb, 56, 0xEF53);
    put16(sb, 58, 1);
    put16(sb, 60, 1);
    put32(sb, 64, now);
    put32(sb, 76, 1);
    put32(sb, 84, FIRST_INO);
    put16(sb, 88, INODE_SIZE);
    put32(sb, 96, 0x0002);
    put32(sb, 100, 0x0003);
    int rnd = open("/dev/urandom", O_RDONLY);
    if (rnd < 0)
        rnd = open("/dev/random", O_RDONLY);
    if (rnd < 0 || read(rnd, sb + 104, 16) != 16) {
        uint64_t x = ((uint64_t)now << 32) ^ (uint64_t)(uintptr_t)&x ^ blocks;
        for (int k = 0; k < 16; k++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            sb[104 + k] = (uint8_t)(x >> 56);
        }
    }
    if (rnd >= 0)
        close(rnd);
    sb[104 + 6] = (uint8_t)((sb[104 + 6] & 0x0F) | 0x40);
    sb[104 + 8] = (uint8_t)((sb[104 + 8] & 0x3F) | 0x80);
    memcpy(sb + 120, label, strlen(label));
    put32(sb, 264, now);

    for (uint32_t g = 0; g < groups; g++) {
        if (!gr[g].super)
            continue;
        put16(sb, 90, g);
        uint64_t sb_off = g == 0 ? 1024 : (uint64_t)gr[g].start * bs;
        if (put(sb_off, sb, sizeof(sb)) < 0 || put((uint64_t)(gr[g].start + 1) * bs, gdt, (size_t)gdt_blocks * bs) < 0)
            return die("erro de escrita no superbloco");
    }

    memset(blk, 0, bs);
    dirent(blk, 0, ROOT_INO, 12, ".", 2);
    dirent(blk, 12, ROOT_INO, 12, "..", 2);
    dirent(blk, 24, FIRST_INO, bs - 24, "lost+found", 2);
    if (put((uint64_t)root_blk * bs, blk, bs) < 0)
        return die("erro de escrita na raiz");
    for (uint32_t k = 0; k < lpf_n; k++) {
        memset(blk, 0, bs);
        if (k == 0) {
            dirent(blk, 0, FIRST_INO, 12, ".", 2);
            dirent(blk, 12, ROOT_INO, bs - 12, "..", 2);
        } else {
            put16(blk, 4, bs);
        }
        if (put((uint64_t)lpf_blk[k] * bs, blk, bs) < 0)
            return die("erro de escrita no lost+found");
    }

    uint8_t raw[INODE_SIZE];
    inode(raw, 0040755, 3, bs, &root_blk, 1, now);
    if (put((uint64_t)gr[0].it * bs + (ROOT_INO - 1) * INODE_SIZE, raw, INODE_SIZE) < 0)
        return die("erro de escrita no inode da raiz");
    inode(raw, 0040700, 2, lpf_n * bs, lpf_blk, lpf_n, now);
    if (put((uint64_t)gr[0].it * bs + (FIRST_INO - 1) * INODE_SIZE, raw, INODE_SIZE) < 0)
        return die("erro de escrita no inode do lost+found");

    if (fsync(fd) < 0)
        return die(orion_strerror(errno));
    close(fd);
    sync();
    printf("mkfs.ext2: %s pronto: %u blocos livres, %u inodes livres, rotulo '%s'\n", dev, free_blocks, free_inodes,
           label);
    return 0;
}
