#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <orion.h>

#define ROOT_INO        2
#define MAX_SHOWN       60
#define INCOMPAT_FILETYPE 0x0002
#define RO_SPARSE       0x0001
#define RO_LARGE        0x0002
#define COMPAT_RESIZE   0x0010
#define S_TYPE          0170000
#define T_DIR           0040000
#define T_REG           0100000
#define T_LNK           0120000
#define T_CHR           0020000
#define T_BLK           0060000
#define T_FIFO          0010000
#define T_SOCK          0140000
#define EA_MAGIC        0xEA020000u

struct grp {
    uint32_t bb, ib, it, free_b, free_i, dirs;
};

static const char *dev;
static int fd, fix;
static uint8_t sb[1024];
static uint32_t bs, blocks, inodes, first_data, bpg, ipg, isz, first_ino, groups, gdt_blocks, resv_gdt, itb;
static uint32_t compat, ro_compat, now;
static int filetype, sparse, large_file;
static struct grp *gr;
static uint8_t *bmap_disk, *imap_disk, *bused, *meta, *alive, *ea_seen, *slot;
static uint16_t *mode, *links;
static uint32_t *refs, *parent, *dotdot;
static long problems, unfixed, shown;

static uint16_t le16(const uint8_t *p, uint32_t off)
{
    return (uint16_t)(p[off] | p[off + 1] << 8);
}

static uint32_t le32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | (uint32_t)p[off + 1] << 8 | (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;
}

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

static int bit(const uint8_t *m, uint64_t i)
{
    return (m[i / 8] >> (i % 8)) & 1;
}

static void set(uint8_t *m, uint64_t i)
{
    m[i / 8] |= (uint8_t)(1u << (i % 8));
}

static int fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fsck.ext2: %s: ", dev);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(8);
}

static int problem(int fixable, const char *fmt, ...)
{
    problems++;
    int fixed = fixable && fix;
    if (!fixed)
        unfixed++;
    if (shown < MAX_SHOWN) {
        va_list ap;
        va_start(ap, fmt);
        printf("  ");
        vprintf(fmt, ap);
        printf("%s\n", fixed ? " (corrigido)" : fixable ? "" : " (nao corrigido)");
        va_end(ap);
    } else if (shown == MAX_SHOWN) {
        printf("  ... mais problemas omitidos\n");
    }
    shown++;
    return fixed;
}

static void rd(uint64_t off, void *buf, size_t len)
{
    uint8_t *p = buf;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
        fatal("erro posicionando no byte %llu", (unsigned long long)off);
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0)
            fatal("erro lendo o byte %llu", (unsigned long long)off);
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
}

static void wr(uint64_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    if (lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
        fatal("erro posicionando no byte %llu", (unsigned long long)off);
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0)
            fatal("erro gravando o byte %llu", (unsigned long long)off);
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
}

static void *zalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        fatal("sem memoria (%zu bytes)", n);
    return p;
}

static int has_super(uint32_t g)
{
    if (g <= 1 || !sparse)
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

static uint32_t gstart(uint32_t g)
{
    return first_data + g * bpg;
}

static uint32_t glen(uint32_t g)
{
    uint64_t left = (uint64_t)blocks - gstart(g);
    return left < bpg ? (uint32_t)left : bpg;
}

static uint64_t ino_off(uint32_t ino)
{
    uint32_t g = (ino - 1) / ipg, i = (ino - 1) % ipg;
    return (uint64_t)gr[g].it * bs + (uint64_t)i * isz;
}

static void read_inode(uint32_t ino, uint8_t *raw)
{
    rd(ino_off(ino), raw, 128);
}

static void write_inode(uint32_t ino, const uint8_t *raw)
{
    wr(ino_off(ino), raw, 128);
}

static int ok_block(uint32_t b)
{
    return b >= first_data && b < blocks && !bit(meta, b);
}

static uint8_t ftype(uint32_t m)
{
    switch (m & S_TYPE) {
    case T_REG: return 1;
    case T_DIR: return 2;
    case T_CHR: return 3;
    case T_BLK: return 4;
    case T_FIFO: return 5;
    case T_SOCK: return 6;
    case T_LNK: return 7;
    default: return 0;
    }
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
        const char *s = strrchr(src, '/');
        int devlike = !s || strncmp(src, "/dev/", 5) == 0;
        s = s ? s + 1 : src;
        busy = strcmp(src, dev) == 0 || (devlike && strcmp(s, base) == 0);
    }
    fclose(m);
    return busy;
}

struct walk {
    uint32_t ino;
    int soft;
    uint64_t count;
    uint64_t last;
    int bad;
};

static int claim(struct walk *w, uint32_t b)
{
    if (w->soft) {
        if (b < first_data || b >= blocks)
            return -1;
        w->count++;
        if (bit(bused, b))
            return 2;
        set(bused, b);
        return 1;
    }
    if (!ok_block(b)) {
        w->bad = 1;
        return problem(1, "inode %u: aponta para o bloco %u, fora da area de dados", w->ino, b) ? 0 : -1;
    }
    if (bit(bused, b)) {
        problem(0, "inode %u: bloco %u usado por mais de um dono", w->ino, b);
        w->count++;
        return 2;
    }
    set(bused, b);
    w->count++;
    return 1;
}

static void walk_table(struct walk *w, uint32_t table, uint32_t depth, uint64_t base)
{
    uint32_t per = bs / 4;
    uint8_t *buf = malloc(bs);
    if (!buf)
        fatal("sem memoria");
    rd((uint64_t)table * bs, buf, bs);
    uint64_t span = 1;
    for (uint32_t d = 1; d < depth; d++)
        span *= per;
    int dirty = 0;
    for (uint32_t i = 0; i < per; i++) {
        uint32_t b = le32(buf, i * 4);
        if (!b)
            continue;
        int r = claim(w, b);
        if (r == 0) {
            put32(buf, i * 4, 0);
            dirty = 1;
            continue;
        }
        if (r < 0)
            continue;
        uint64_t idx = base + i * span;
        if (depth == 1) {
            if (idx + 1 > w->last)
                w->last = idx + 1;
        } else if (r == 1) {
            walk_table(w, b, depth - 1, idx);
        }
    }
    if (dirty)
        wr((uint64_t)table * bs, buf, bs);
    free(buf);
}

static uint64_t max_size(void)
{
    uint64_t per = bs / 4;
    uint64_t n = 12 + per + per * per + per * per * per;
    uint64_t limit = large_file ? (1ull << 63) - 1 : 0xFFFFFFFFull;
    return n > limit / bs ? limit : n * bs;
}

static int inline_link(const uint8_t *raw)
{
    uint32_t ea = le32(raw, 104) ? bs / 512 : 0;
    uint32_t sectors = le32(raw, 28);
    return le32(raw, 4) <= 60 && (sectors < ea ? 0 : sectors - ea) == 0;
}

static void clear_inode(uint32_t ino, uint8_t *raw)
{
    put16(raw, 26, 0);
    put32(raw, 20, now);
    write_inode(ino, raw);
}

static void check_inode(uint32_t ino, uint8_t *raw)
{
    uint32_t m = le16(raw, 0), n = le16(raw, 26), dtime = le32(raw, 20);
    int reserved = ino < first_ino && ino != ROOT_INO;
    int in_map = bit(imap_disk, ino - 1);
    if (ino == ROOT_INO && (m & S_TYPE) != T_DIR)
        fatal("o inode da raiz nao e um diretorio");
    if (reserved) {
        set(alive, ino);
    } else if (n == 0 && ino != ROOT_INO) {
        if (m != 0 && dtime <= inodes) {
            if (problem(1, in_map ? "inode %u: sem links mas marcado em uso (orfao)"
                                  : "inode %u: apagado mas sem hora de remocao", ino))
                clear_inode(ino, raw);
        }
        return;
    } else {
        if (!ftype(m)) {
            if (problem(1, "inode %u: tipo de arquivo invalido (modo %o)", ino, m))
                clear_inode(ino, raw);
            return;
        }
        if (dtime) {
            if (problem(1, "inode %u: em uso mas com hora de remocao", ino)) {
                put32(raw, 20, 0);
                write_inode(ino, raw);
            }
        }
        set(alive, ino);
        mode[ino] = (uint16_t)m;
        links[ino] = (uint16_t)n;
        if (le32(raw, 112) || le16(raw, 116) || le16(raw, 118)) {
            if (problem(1, "inode %u: campos que o ext2 nao usa tem lixo", ino)) {
                put32(raw, 112, 0);
                put32(raw, 116, 0);
                write_inode(ino, raw);
            }
        }
        if ((m & S_TYPE) == T_DIR && le32(raw, 108)) {
            if (problem(1, "diretorio %u: parte alta do tamanho tem lixo", ino)) {
                put32(raw, 108, 0);
                write_inode(ino, raw);
            }
        }
    }
    uint32_t type = m & S_TYPE;
    int has_blocks = reserved || type == T_REG || type == T_DIR || (type == T_LNK && !inline_link(raw));
    struct walk w = { .ino = ino, .soft = reserved };
    int dirty = 0;
    if (has_blocks) {
        for (uint32_t k = 0; k < 15; k++) {
            uint32_t b = le32(raw, 40 + 4 * k);
            if (!b)
                continue;
            int r = claim(&w, b);
            if (r == 0) {
                put32(raw, 40 + 4 * k, 0);
                dirty = 1;
                continue;
            }
            if (r < 0)
                continue;
            if (k < 12) {
                if (k + 1 > w.last)
                    w.last = k + 1;
            } else if (r == 1) {
                uint64_t per = bs / 4, base = 12;
                if (k >= 13)
                    base += per;
                if (k == 14)
                    base += per * per;
                walk_table(&w, b, k - 11, base);
            }
        }
    }
    uint32_t acl = le32(raw, 104);
    if (acl && !reserved) {
        uint8_t hdr[8];
        if (!ok_block(acl) || (rd((uint64_t)acl * bs, hdr, 8), le32(hdr, 0) != EA_MAGIC)) {
            if (problem(1, "inode %u: bloco de atributos %u invalido", ino, acl)) {
                put32(raw, 104, 0);
                dirty = 1;
            }
        } else {
            if (bit(bused, acl) && !bit(ea_seen, acl))
                problem(0, "inode %u: bloco de atributos %u usado tambem como dado", ino, acl);
            set(bused, acl);
            set(ea_seen, acl);
            w.count++;
        }
    }
    if (reserved) {
        if (dirty)
            write_inode(ino, raw);
        return;
    }

    uint64_t size = le32(raw, 4);
    if (type == T_REG && large_file)
        size |= (uint64_t)le32(raw, 108) << 32;
    uint64_t need = (size + bs - 1) / bs;
    if (type == T_DIR) {
        if (size % bs || size == 0) {
            uint64_t fixed = w.last * bs;
            if (problem(fixed != 0, "diretorio %u: tamanho %llu nao e multiplo do bloco", ino,
                        (unsigned long long)size) && fixed) {
                put32(raw, 4, (uint32_t)fixed);
                dirty = 1;
                size = fixed;
            }
        } else if (w.last > need) {
            if (problem(1, "diretorio %u: blocos alem do tamanho", ino)) {
                put32(raw, 4, (uint32_t)(w.last * bs));
                dirty = 1;
            }
        }
    } else if ((type == T_CHR || type == T_BLK || type == T_FIFO || type == T_SOCK) && size) {
        if (problem(1, "inode %u: arquivo especial com tamanho %llu", ino, (unsigned long long)size)) {
            put32(raw, 4, 0);
            dirty = 1;
        }
    } else if (type == T_REG && size > max_size()) {
        uint64_t fixed = w.last * bs;
        if (problem(1, "inode %u: tamanho %llu alem do que o ext2 enderecar", ino, (unsigned long long)size)) {
            put32(raw, 4, (uint32_t)fixed);
            put32(raw, 108, (uint32_t)(fixed >> 32));
            dirty = 1;
        }
    } else if (has_blocks && w.last > need) {
        uint64_t fixed = w.last * bs;
        if (problem(1, "inode %u: tamanho %llu menor que os blocos que usa", ino, (unsigned long long)size)) {
            put32(raw, 4, (uint32_t)fixed);
            if (type == T_REG && large_file)
                put32(raw, 108, (uint32_t)(fixed >> 32));
            dirty = 1;
        }
    }
    uint64_t sectors = w.count * (bs / 512);
    if (le32(raw, 28) != (uint32_t)sectors) {
        if (problem(1, "inode %u: i_blocks e %u, deveria ser %llu", ino, le32(raw, 28),
                    (unsigned long long)sectors)) {
            put32(raw, 28, (uint32_t)sectors);
            dirty = 1;
        }
    }
    if (dirty)
        write_inode(ino, raw);
}

static void pass1(void)
{
    uint8_t *table = zalloc((size_t)itb * bs);
    for (uint32_t g = 0; g < groups; g++) {
        rd((uint64_t)gr[g].it * bs, table, (size_t)itb * bs);
        for (uint32_t i = 0; i < ipg; i++) {
            uint32_t ino = g * ipg + i + 1;
            if (ino > inodes)
                break;
            uint8_t raw[128];
            memcpy(raw, table + (size_t)i * isz, 128);
            check_inode(ino, raw);
        }
    }
    free(table);
}

static uint32_t map_block(const uint8_t *raw, uint64_t n)
{
    uint64_t per = bs / 4;
    if (n < 12) {
        uint32_t b = le32(raw, 40 + 4 * (uint32_t)n);
        return ok_block(b) ? b : 0;
    }
    n -= 12;
    uint32_t slot = 12, depth = 1;
    uint64_t span = per;
    while (n >= span) {
        n -= span;
        if (++slot > 14)
            return 0;
        depth++;
        span *= per;
    }
    uint32_t b = le32(raw, 40 + 4 * slot);
    uint64_t unit = span / per;
    for (uint32_t d = 0; d < depth && b; d++) {
        if (b >= blocks)
            return 0;
        uint8_t e[4];
        rd((uint64_t)b * bs + (n / unit) * 4, e, 4);
        b = le32(e, 0);
        n %= unit;
        unit = unit / per ? unit / per : 1;
    }
    return ok_block(b) ? b : 0;
}

static void put_entry(uint8_t *buf, uint32_t pos, uint32_t ino, uint32_t rec, const char *name)
{
    uint32_t len = (uint32_t)strlen(name);
    put32(buf, pos, ino);
    put16(buf, pos + 4, rec);
    if (filetype) {
        buf[pos + 6] = (uint8_t)len;
        buf[pos + 7] = 2;
    } else {
        put16(buf, pos + 6, len);
    }
    memcpy(buf + pos + 8, name, len);
}

static void remove_entry(uint8_t *buf, uint32_t pos, int prev)
{
    if (prev >= 0)
        put16(buf, (uint32_t)prev + 4, le16(buf, (uint32_t)prev + 4) + le16(buf, pos + 4));
    else
        put32(buf, pos, 0);
}

static void check_dir(uint32_t d)
{
    uint8_t raw[128];
    read_inode(d, raw);
    uint32_t size = le32(raw, 4);
    uint8_t *buf = malloc(bs);
    if (!buf)
        fatal("sem memoria");
    uint32_t seen = 0;
    for (uint64_t k = 0; k < size / bs; k++) {
        uint32_t phys = map_block(raw, k);
        if (!phys) {
            problem(0, "diretorio %u: buraco no bloco %llu", d, (unsigned long long)k);
            continue;
        }
        rd((uint64_t)phys * bs, buf, bs);
        int dirty = 0, prev = -1;
        uint32_t pos = 0;
        while (pos < bs) {
            uint32_t e = le32(buf, pos), rec = pos + 8 <= bs ? le16(buf, pos + 4) : 0;
            uint32_t len = pos + 8 <= bs ? (filetype ? buf[pos + 6] : le16(buf, pos + 6)) : 0;
            if (rec < 8 || rec % 4 || pos + rec > bs || len + 8 > rec) {
                if (problem(1, "diretorio %u: entrada corrompida no bloco %llu", d, (unsigned long long)k)) {
                    if (prev >= 0) {
                        put16(buf, (uint32_t)prev + 4, bs - (uint32_t)prev);
                    } else {
                        memset(buf, 0, bs);
                        put16(buf, 4, bs);
                    }
                    dirty = 1;
                }
                break;
            }
            char name[256];
            memcpy(name, buf + pos + 8, len);
            name[len] = 0;
            int first = k == 0 && pos == 0;
            int second = k == 0 && prev == 0 && seen == 1;
            int removed = 0;
            if (first && !e) {
                if (rec >= 24 && problem(1, "diretorio %u: faltam as entradas '.' e '..'", d)) {
                    put_entry(buf, 0, d, 12, ".");
                    put_entry(buf, 12, ROOT_INO, rec - 12, "..");
                    dotdot[d] = ROOT_INO;
                    set(slot, d);
                    refs[d]++;
                    refs[ROOT_INO]++;
                    seen = 2;
                    prev = 12;
                    pos = rec;
                    dirty = 1;
                    continue;
                }
                if (rec < 24)
                    problem(0, "diretorio %u: falta a entrada '.'", d);
            }
            if (second && !e && len == 2 && memcmp(buf + pos + 8, "..", 2) == 0) {
                set(slot, d);
                if (problem(1, "diretorio %u: a entrada '..' esta vazia", d)) {
                    put32(buf, pos, ROOT_INO);
                    e = ROOT_INO;
                    dirty = 1;
                }
            }
            if (e) {
                seen++;
                int dot = strcmp(name, ".") == 0, dotdot_name = strcmp(name, "..") == 0;
                if (first && !dot) {
                    problem(0, "diretorio %u: a primeira entrada nao e '.'", d);
                } else if (first && e != d) {
                    if (problem(1, "diretorio %u: '.' aponta para %u", d, e)) {
                        put32(buf, pos, d);
                        e = d;
                        dirty = 1;
                    }
                }
                if (second && dotdot_name) {
                    dotdot[d] = e;
                    set(slot, d);
                } else if (second) {
                    problem(0, "diretorio %u: a segunda entrada nao e '..'", d);
                }
                if (e > inodes || !bit(alive, e)) {
                    if (problem(1, "diretorio %u: '%s' aponta para o inode %u, que esta livre", d, name, e)) {
                        if (first || second) {
                            put32(buf, pos, first ? d : ROOT_INO);
                            e = first ? d : ROOT_INO;
                            if (second)
                                dotdot[d] = e;
                        } else {
                            remove_entry(buf, pos, prev);
                            removed = 1;
                        }
                        dirty = 1;
                    }
                    if (!removed && (e > inodes || !bit(alive, e))) {
                        prev = (int)pos;
                        pos += rec;
                        continue;
                    }
                } else if (!first && !second && (dot || dotdot_name || len == 0 || memchr(buf + pos + 8, '/', len) ||
                                                 memchr(buf + pos + 8, 0, len))) {
                    if (problem(1, "diretorio %u: nome invalido '%s'", d, name)) {
                        remove_entry(buf, pos, prev);
                        removed = 1;
                        dirty = 1;
                    }
                }
                if (!removed && filetype && buf[pos + 7] != ftype(mode[e]) && !(e < first_ino && e != ROOT_INO)) {
                    if (problem(1, "diretorio %u: tipo errado na entrada '%s'", d, name)) {
                        buf[pos + 7] = ftype(mode[e]);
                        dirty = 1;
                    }
                }
                if (!removed && !first && !second && (mode[e] & S_TYPE) == T_DIR) {
                    if (parent[e]) {
                        if (problem(1, "diretorio %u tem mais de um nome ('%s' em %u)", e, name, d)) {
                            remove_entry(buf, pos, prev);
                            removed = 1;
                            dirty = 1;
                        }
                    } else {
                        parent[e] = d;
                    }
                }
                if (!removed)
                    refs[e]++;
            }
            if (!removed)
                prev = (int)pos;
            pos += rec;
        }
        if (dirty)
            wr((uint64_t)phys * bs, buf, bs);
    }
    free(buf);
}

static void set_dotdot(uint32_t d, uint32_t p)
{
    uint8_t raw[128];
    read_inode(d, raw);
    uint32_t phys = map_block(raw, 0);
    if (!phys)
        return;
    uint8_t *buf = zalloc(bs);
    rd((uint64_t)phys * bs, buf, bs);
    uint32_t pos = le16(buf, 4);
    if (pos + 12 <= bs && le16(buf, pos + 4) >= 12) {
        put32(buf, pos, p);
        wr((uint64_t)phys * bs, buf, bs);
    }
    free(buf);
}

static void pass2_3_4(void)
{
    for (uint32_t ino = 1; ino <= inodes; ino++)
        if (bit(alive, ino) && (ino >= first_ino || ino == ROOT_INO) && (mode[ino] & S_TYPE) == T_DIR)
            check_dir(ino);

    parent[ROOT_INO] = ROOT_INO;
    for (uint32_t ino = 1; ino <= inodes; ino++) {
        if (!bit(alive, ino) || (ino < first_ino && ino != ROOT_INO) || (mode[ino] & S_TYPE) != T_DIR)
            continue;
        uint32_t p = parent[ino];
        if (!p) {
            problem(0, "diretorio %u: desconectado da arvore", ino);
            continue;
        }
        uint32_t cur = ino;
        uint32_t steps = 0;
        while (cur != ROOT_INO && cur && steps++ <= inodes)
            cur = parent[cur];
        if (cur != ROOT_INO) {
            problem(0, "diretorio %u: preso num ciclo fora da arvore", ino);
            continue;
        }
        if (dotdot[ino] != p && !bit(slot, ino)) {
            problem(0, "diretorio %u: sem a entrada '..'", ino);
        } else if (dotdot[ino] != p) {
            if (problem(1, "diretorio %u: '..' aponta para %u, deveria ser %u", ino, dotdot[ino], p)) {
                set_dotdot(ino, p);
                if (dotdot[ino] && dotdot[ino] <= inodes && refs[dotdot[ino]])
                    refs[dotdot[ino]]--;
                refs[p]++;
                dotdot[ino] = p;
            }
        }
    }

    for (uint32_t ino = 1; ino <= inodes; ino++) {
        if (!bit(alive, ino) || (ino < first_ino && ino != ROOT_INO))
            continue;
        if (refs[ino] == 0) {
            problem(0, "inode %u: em uso mas sem nenhum nome", ino);
            continue;
        }
        if (refs[ino] != links[ino]) {
            if (problem(1, "inode %u: links_count e %u, deveria ser %u", ino, links[ino], refs[ino])) {
                uint8_t raw[128];
                read_inode(ino, raw);
                put16(raw, 26, refs[ino] > 0xFFFF ? 0xFFFF : refs[ino]);
                write_inode(ino, raw);
            }
        }
    }
}

static void pass5(void)
{
    uint32_t total_fb = 0, total_fi = 0;
    uint8_t *gdt = zalloc((size_t)gdt_blocks * bs);
    rd((uint64_t)(first_data + 1) * bs, gdt, (size_t)groups * 32);
    int gdt_dirty = 0;
    for (uint32_t g = 0; g < groups; g++) {
        uint8_t *dm = bmap_disk + (size_t)g * bs;
        uint8_t *want = zalloc(bs);
        uint32_t len = glen(g), fb = 0, diff = 0;
        for (uint32_t i = 0; i < 8 * bs; i++) {
            if (i >= len || bit(bused, (uint64_t)gstart(g) + i))
                set(want, i);
            else
                fb++;
            if (bit(want, i) != bit(dm, i))
                diff++;
        }
        if (diff && problem(1, "grupo %u: bitmap de blocos difere em %u bloco(s)", g, diff))
            wr((uint64_t)gr[g].bb * bs, want, bs);

        uint8_t *im = imap_disk + (size_t)g * bs;
        memset(want, 0, bs);
        uint32_t fi = 0, dirs = 0;
        diff = 0;
        for (uint32_t i = 0; i < 8 * bs; i++) {
            uint32_t ino = g * ipg + i + 1;
            int used = i >= ipg || ino < first_ino || bit(alive, ino);
            if (used)
                set(want, i);
            else
                fi++;
            if (i < ipg && ino >= first_ino && bit(alive, ino) && (mode[ino] & S_TYPE) == T_DIR)
                dirs++;
            if (i < ipg && ino == ROOT_INO)
                dirs++;
            if (bit(want, i) != bit(im, i))
                diff++;
        }
        if (diff && problem(1, "grupo %u: bitmap de inodes difere em %u inode(s)", g, diff))
            wr((uint64_t)gr[g].ib * bs, want, bs);
        free(want);

        if (gr[g].free_b != fb && problem(1, "grupo %u: blocos livres %u, deveria ser %u", g, gr[g].free_b, fb)) {
            put16(gdt, g * 32 + 12, fb);
            gdt_dirty = 1;
        }
        if (gr[g].free_i != fi && problem(1, "grupo %u: inodes livres %u, deveria ser %u", g, gr[g].free_i, fi)) {
            put16(gdt, g * 32 + 14, fi);
            gdt_dirty = 1;
        }
        if (gr[g].dirs != dirs && problem(1, "grupo %u: diretorios %u, deveria ser %u", g, gr[g].dirs, dirs)) {
            put16(gdt, g * 32 + 16, dirs);
            gdt_dirty = 1;
        }
        total_fb += fb;
        total_fi += fi;
    }
    if (gdt_dirty)
        wr((uint64_t)(first_data + 1) * bs, gdt, (size_t)groups * 32);
    free(gdt);
    int sb_dirty = 0;
    if (le32(sb, 12) != total_fb && problem(1, "superbloco: blocos livres %u, deveria ser %u", le32(sb, 12), total_fb)) {
        put32(sb, 12, total_fb);
        sb_dirty = 1;
    }
    if (le32(sb, 16) != total_fi && problem(1, "superbloco: inodes livres %u, deveria ser %u", le32(sb, 16), total_fi)) {
        put32(sb, 16, total_fi);
        sb_dirty = 1;
    }
    if (le32(sb, 8) > blocks && problem(1, "superbloco: %u blocos reservados, mais que o total", le32(sb, 8))) {
        put32(sb, 8, 0);
        sb_dirty = 1;
    }
    uint16_t state = le16(sb, 58);
    if (fix && !unfixed && (state != 1 || sb_dirty)) {
        put16(sb, 58, 1);
        put32(sb, 64, now);
        sb_dirty = 1;
    }
    if (sb_dirty && fix)
        wr(1024, sb, sizeof(sb));
    printf("fsck.ext2: %s: %u/%u inodes, %u/%u blocos em uso\n", dev, inodes - total_fi, inodes, blocks - total_fb,
           blocks);
}

static void load(void)
{
    rd(1024, sb, sizeof(sb));
    if (le16(sb, 56) != 0xEF53)
        fatal("nao e um ext2 (assinatura errada)");
    uint32_t log = le32(sb, 24);
    if (log > 2)
        fatal("tamanho de bloco nao suportado");
    bs = 1024u << log;
    inodes = le32(sb, 0);
    blocks = le32(sb, 4);
    first_data = le32(sb, 20);
    bpg = le32(sb, 32);
    ipg = le32(sb, 40);
    uint32_t rev = le32(sb, 76);
    isz = rev >= 1 ? le16(sb, 88) : 128;
    first_ino = rev >= 1 ? le32(sb, 84) : 11;
    compat = rev >= 1 ? le32(sb, 92) : 0;
    uint32_t incompat = rev >= 1 ? le32(sb, 96) : 0;
    ro_compat = rev >= 1 ? le32(sb, 100) : 0;
    if (incompat & ~(uint32_t)INCOMPAT_FILETYPE)
        fatal("recursos incompativeis 0x%x nao suportados (ext3/ext4?)", incompat);
    if (ro_compat & ~(uint32_t)(RO_SPARSE | RO_LARGE))
        fatal("recursos ro_compat 0x%x nao suportados", ro_compat);
    filetype = (incompat & INCOMPAT_FILETYPE) != 0;
    sparse = (ro_compat & RO_SPARSE) != 0;
    large_file = (ro_compat & RO_LARGE) != 0;
    if (bpg < 256 || bpg > 8 * bs || ipg == 0 || ipg > 8 * bs || blocks <= first_data || first_data > 1 ||
        isz < 128 || isz > bs || (isz & (isz - 1)) || first_ino <= ROOT_INO || first_ino > inodes)
        fatal("superbloco com valores impossiveis");
    groups = (blocks - first_data + bpg - 1) / bpg;
    if ((uint64_t)groups * ipg != inodes)
        fatal("superbloco: %u inodes nao batem com %u grupo(s) de %u", inodes, groups, ipg);
    struct stat st;
    uint64_t size = 0;
    if (fstat(fd, &st) == 0)
        size = (uint64_t)st.st_size;
    if (!size) {
        off_t end = lseek(fd, 0, SEEK_END);
        size = end > 0 ? (uint64_t)end : 0;
    }
    if (size && (uint64_t)blocks * bs > size)
        fatal("o superbloco declara %u blocos, mais do que o dispositivo tem", blocks);
    gdt_blocks = (groups * 32 + bs - 1) / bs;
    resv_gdt = (compat & COMPAT_RESIZE) ? le16(sb, 206) : 0;
    itb = (uint32_t)(((uint64_t)ipg * isz + bs - 1) / bs);

    gr = zalloc(sizeof(*gr) * groups);
    uint8_t *gdt = zalloc((size_t)groups * 32);
    rd((uint64_t)(first_data + 1) * bs, gdt, (size_t)groups * 32);
    meta = zalloc(blocks / 8 + 1);
    bused = zalloc(blocks / 8 + 1);
    ea_seen = zalloc(blocks / 8 + 1);
    bmap_disk = zalloc((size_t)groups * bs);
    imap_disk = zalloc((size_t)groups * bs);
    for (uint32_t g = 0; g < groups; g++) {
        struct grp *p = &gr[g];
        p->bb = le32(gdt, g * 32);
        p->ib = le32(gdt, g * 32 + 4);
        p->it = le32(gdt, g * 32 + 8);
        p->free_b = le16(gdt, g * 32 + 12);
        p->free_i = le16(gdt, g * 32 + 14);
        p->dirs = le16(gdt, g * 32 + 16);
        uint32_t s = gstart(g), e = s + glen(g);
        if (p->bb < s || p->bb >= e || p->ib < s || p->ib >= e || p->it < s || (uint64_t)p->it + itb > e)
            fatal("descritor do grupo %u aponta para fora do grupo", g);
        if (has_super(g)) {
            uint64_t end = (uint64_t)s + 1 + gdt_blocks + resv_gdt;
            for (uint64_t b = s; b < end && b < e; b++)
                set(meta, b);
        }
        set(meta, p->bb);
        set(meta, p->ib);
        for (uint32_t b = 0; b < itb; b++)
            set(meta, (uint64_t)p->it + b);
        rd((uint64_t)p->bb * bs, bmap_disk + (size_t)g * bs, bs);
        rd((uint64_t)p->ib * bs, imap_disk + (size_t)g * bs, bs);
    }
    free(gdt);
    memcpy(bused, meta, blocks / 8 + 1);

    size_t n = (size_t)inodes + 1;
    alive = zalloc(n / 8 + 1);
    slot = zalloc(n / 8 + 1);
    mode = zalloc(n * sizeof(*mode));
    links = zalloc(n * sizeof(*links));
    refs = zalloc(n * sizeof(*refs));
    parent = zalloc(n * sizeof(*parent));
    dotdot = zalloc(n * sizeof(*dotdot));
}

static int usage(void)
{
    fprintf(stderr, "uso: fsck.ext2 [-n | -y] [-f] <dispositivo>\n");
    fprintf(stderr, "     -n so confere (padrao), -y corrige o que for seguro\n");
    return 8;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        for (const char *c = argv[i] + 1; *c; c++) {
            if (*c == 'y' || *c == 'p')
                fix = 1;
            else if (*c == 'n')
                fix = 0;
            else if (*c != 'f')
                return usage();
        }
    }
    if (argc - i != 1)
        return usage();
    dev = argv[i];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (mounted())
        fatal("esta montado; desmonte antes de conferir");
    fd = open(dev, fix ? O_RDWR : O_RDONLY);
    if (fd < 0)
        fatal("%s", orion_strerror(errno));
    now = (uint32_t)time(NULL);
    load();
    uint16_t state = le16(sb, 58);
    printf("fsck.ext2: %s: %u blocos de %u bytes, %u grupo(s)%s\n", dev, blocks, bs, groups,
           (state & 1) ? "" : ", nao foi desmontado direito");
    if (state & 2)
        printf("  o superbloco tem a marca de erro\n");
    pass1();
    pass2_3_4();
    pass5();
    if (fix)
        fsync(fd);
    close(fd);
    if (fix)
        sync();
    if (!problems) {
        printf("fsck.ext2: %s: limpo\n", dev);
        return 0;
    }
    if (!unfixed) {
        printf("fsck.ext2: %s: %ld problema(s), todos corrigidos\n", dev, problems);
        return 1;
    }
    printf("fsck.ext2: %s: %ld problema(s), %ld nao corrigido(s)%s\n", dev, problems, unfixed,
           fix ? "" : " (rode com -y para corrigir)");
    return 4;
}
