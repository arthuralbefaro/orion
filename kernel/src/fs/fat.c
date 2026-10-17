#include <fs/fat.h>
#include <fs/vfs.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <mm/heap.h>
#include <lib/string.h>
#include <drivers/rtc.h>

enum fat_type { FAT12, FAT16, FAT32 };

struct fat_fs {
    struct blockdev *dev;
    enum fat_type type;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_lba;
    uint32_t fat_sectors;
    uint32_t root_lba;
    uint32_t root_sectors;
    uint32_t root_cluster;
    uint32_t data_lba;
    uint32_t total_clusters;
    uint8_t *fat;
    uint8_t *cluster_buf;
    uint8_t *sector_buf;
    uint32_t fat_copies;
    uint32_t fat_dirty_lo;
    uint32_t fat_dirty_hi;
    uint32_t next_free;
    bool writable;
    struct mutex lock;
    char lock_name[16];
    char label[12];
};

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_LFN       0x0F

struct fat_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} PACKED;

static uint32_t fat_next(struct fat_fs *fs, uint32_t cluster)
{
    switch (fs->type) {
    case FAT12: {
        uint32_t off = cluster + cluster / 2;
        uint16_t v;
        memcpy(&v, fs->fat + off, 2);
        v = (cluster & 1) ? v >> 4 : v & 0x0FFF;
        return v >= 0xFF8 ? 0xFFFFFFFF : v;
    }
    case FAT16: {
        uint16_t v;
        memcpy(&v, fs->fat + cluster * 2, 2);
        return v >= 0xFFF8 ? 0xFFFFFFFF : v;
    }
    default: {
        uint32_t v;
        memcpy(&v, fs->fat + cluster * 4, 4);
        v &= 0x0FFFFFFF;
        return v >= 0x0FFFFFF8 ? 0xFFFFFFFF : v;
    }
    }
}

static uint32_t cluster_lba(struct fat_fs *fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2) * fs->sectors_per_cluster;
}

static bool read_cluster(struct fat_fs *fs, uint32_t cluster, void *buf)
{
    if (cluster < 2 || cluster >= fs->total_clusters + 2)
        return false;
    return blockdev_read(fs->dev, cluster_lba(fs, cluster), fs->sectors_per_cluster, buf);
}

struct lfn_state {
    char name[VFS_NAME_MAX];
    uint8_t checksum;
    bool valid;
};

static void lfn_add(struct lfn_state *lfn, const uint8_t *e)
{
    uint8_t seq = e[0] & 0x1F;
    if (e[0] & 0x40) {
        memset(lfn->name, 0, sizeof(lfn->name));
        lfn->checksum = e[13];
        lfn->valid = true;
    }
    if (!lfn->valid || seq == 0 || seq > 20 || e[13] != lfn->checksum)
        return;

    static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    size_t base = (size_t)(seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint16_t ch;
        memcpy(&ch, e + offs[i], 2);
        if (base + i >= VFS_NAME_MAX - 1)
            break;
        if (ch == 0 || ch == 0xFFFF)
            lfn->name[base + i] = '\0';
        else
            lfn->name[base + i] = ch < 0x80 ? (char)ch : '?';
    }
}

static uint8_t short_checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + name[i];
    return sum;
}

static void short_name(const struct fat_dirent *d, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && d->name[i] != ' '; i++)
        out[n++] = (char)d->name[i];
    if (d->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && d->name[i] != ' '; i++)
            out[n++] = (char)d->name[i];
    }
    out[n] = '\0';
    if (out[0] == 0x05)
        out[0] = (char)0xE5;
}

static void fill_node(struct filesystem *fs, const struct fat_dirent *d, const char *name, struct vfs_node *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, name, sizeof(out->name));
    out->is_dir = d->attr & ATTR_DIR;
    out->size = out->is_dir ? 0 : d->size;
    out->ino = ((uint32_t)d->cluster_hi << 16) | d->cluster_lo;
    out->fs = fs;
}

typedef bool (*dirent_cb)(struct filesystem *fs, const struct fat_dirent *d, const char *name,
                          uint32_t dir_offset, void *ctx);

static bool scan_block(struct filesystem *fs, const uint8_t *buf, uint32_t size, struct lfn_state *lfn,
                       dirent_cb cb, void *ctx, bool *stop, uint32_t base_offset)
{
    for (uint32_t off = 0; off + 32 <= size; off += 32) {
        const uint8_t *e = buf + off;
        if (e[0] == 0x00) {
            *stop = true;
            return false;
        }
        if (e[0] == 0xE5)
            continue;
        const struct fat_dirent *d = (const void *)e;
        if (d->attr == ATTR_LFN) {
            lfn_add(lfn, e);
            continue;
        }
        if (d->attr & ATTR_VOLUME) {
            lfn->valid = false;
            continue;
        }
        char sname[13];
        short_name(d, sname);
        const char *name = sname;
        if (lfn->valid && lfn->checksum == short_checksum(d->name) && lfn->name[0])
            name = lfn->name;
        lfn->valid = false;
        if (cb(fs, d, name, base_offset + off, ctx)) {
            *stop = true;
            return true;
        }
    }
    return false;
}

static bool iterate_dir(struct filesystem *fs, const struct vfs_node *dir, dirent_cb cb, void *ctx)
{
    struct fat_fs *f = fs->priv;
    struct lfn_state lfn = { .valid = false };
    bool stop = false, found = false;

    mutex_lock(&f->lock);
    if (dir->ino == 0 && f->type != FAT32) {
        for (uint32_t s = 0; s < f->root_sectors && !stop; s++) {
            if (!blockdev_read(f->dev, f->root_lba + s, 1, f->cluster_buf))
                break;
            found = scan_block(fs, f->cluster_buf, f->bytes_per_sector, &lfn, cb, ctx, &stop,
                               s * f->bytes_per_sector);
        }
    } else {
        uint32_t c = dir->ino == 0 ? f->root_cluster : (uint32_t)dir->ino;
        uint32_t guard = 0, base = 0;
        while (c != 0xFFFFFFFF && !stop && guard++ < f->total_clusters) {
            if (!read_cluster(f, c, f->cluster_buf))
                break;
            found = scan_block(fs, f->cluster_buf, f->cluster_size, &lfn, cb, ctx, &stop, base);
            base += f->cluster_size;
            c = fat_next(f, c);
        }
    }
    mutex_unlock(&f->lock);
    return found;
}

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

struct lookup_ctx {
    const char *name;
    struct vfs_node *out;
};

static bool lookup_cb(struct filesystem *fs, const struct fat_dirent *d, const char *name,
                      uint32_t dir_offset, void *ctx)
{
    struct lookup_ctx *l = ctx;
    if (!str_ieq(name, l->name))
        return false;
    fill_node(fs, d, name, l->out);
    l->out->dir_offset = dir_offset;
    if (l->out->is_dir && l->out->ino == 0 && ((struct fat_fs *)fs->priv)->type == FAT32)
        l->out->ino = 0;
    return true;
}

static bool fat_root(struct filesystem *fs, struct vfs_node *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, "/", sizeof(out->name));
    out->is_dir = true;
    out->ino = 0;
    out->fs = fs;
    return true;
}

static bool fat_lookup(struct filesystem *fs, const struct vfs_node *dir, const char *name, struct vfs_node *out)
{
    if (strcmp(name, ".") == 0) {
        *out = *dir;
        return true;
    }
    struct lookup_ctx ctx = { .name = name, .out = out };
    if (!iterate_dir(fs, dir, lookup_cb, &ctx))
        return false;
    out->dir_ino = dir->ino;
    return true;
}

struct readdir_ctx {
    vfs_dir_cb cb;
    void *ctx;
};

static bool readdir_cb(struct filesystem *fs, const struct fat_dirent *d, const char *name,
                       uint32_t dir_offset, void *ctx)
{
    struct readdir_ctx *r = ctx;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    struct vfs_node n;
    fill_node(fs, d, name, &n);
    n.dir_offset = dir_offset;
    return !r->cb(&n, r->ctx);
}

static bool fat_readdir(struct filesystem *fs, const struct vfs_node *dir, vfs_dir_cb cb, void *ctx)
{
    struct readdir_ctx r = { .cb = cb, .ctx = ctx };
    iterate_dir(fs, dir, readdir_cb, &r);
    return true;
}

static int64_t fat_read(struct filesystem *fs, const struct vfs_node *node, uint64_t offset, void *buf, uint64_t len)
{
    struct fat_fs *f = fs->priv;
    if (offset >= node->size)
        return 0;
    if (offset + len > node->size)
        len = node->size - offset;

    uint8_t *out = buf;
    uint64_t done = 0;
    mutex_lock(&f->lock);

    uint32_t c = (uint32_t)node->ino;
    uint64_t skip = offset / f->cluster_size;
    while (skip-- && c != 0xFFFFFFFF)
        c = fat_next(f, c);
    uint64_t in_cluster = offset % f->cluster_size;

    while (done < len && c != 0xFFFFFFFF) {
        if (!read_cluster(f, c, f->cluster_buf))
            break;
        uint64_t chunk = f->cluster_size - in_cluster;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(out + done, f->cluster_buf + in_cluster, chunk);
        done += chunk;
        in_cluster = 0;
        c = fat_next(f, c);
    }
    mutex_unlock(&f->lock);
    return (int64_t)done;
}

#define FAT_EOC 0x0FFFFFFF

static void fat_mark_dirty(struct fat_fs *f, uint32_t byte_off)
{
    uint32_t sector = byte_off / f->bytes_per_sector;
    if (f->fat_dirty_lo > f->fat_dirty_hi) {
        f->fat_dirty_lo = f->fat_dirty_hi = sector;
        return;
    }
    if (sector < f->fat_dirty_lo)
        f->fat_dirty_lo = sector;
    if (sector > f->fat_dirty_hi)
        f->fat_dirty_hi = sector;
}

static uint32_t fat_get_raw(struct fat_fs *f, uint32_t cluster)
{
    switch (f->type) {
    case FAT12: {
        uint32_t off = cluster + cluster / 2;
        uint16_t v;
        memcpy(&v, f->fat + off, 2);
        return (cluster & 1) ? (uint32_t)(v >> 4) : (uint32_t)(v & 0x0FFF);
    }
    case FAT16: {
        uint16_t v;
        memcpy(&v, f->fat + cluster * 2, 2);
        return v;
    }
    default: {
        uint32_t v;
        memcpy(&v, f->fat + cluster * 4, 4);
        return v & 0x0FFFFFFF;
    }
    }
}

static void fat_set(struct fat_fs *f, uint32_t cluster, uint32_t value)
{
    switch (f->type) {
    case FAT12: {
        uint32_t off = cluster + cluster / 2;
        uint16_t v;
        memcpy(&v, f->fat + off, 2);
        if (cluster & 1)
            v = (uint16_t)((v & 0x000F) | ((value & 0x0FFF) << 4));
        else
            v = (uint16_t)((v & 0xF000) | (value & 0x0FFF));
        memcpy(f->fat + off, &v, 2);
        fat_mark_dirty(f, off);
        fat_mark_dirty(f, off + 1);
        break;
    }
    case FAT16: {
        uint16_t v = (uint16_t)value;
        memcpy(f->fat + cluster * 2, &v, 2);
        fat_mark_dirty(f, cluster * 2);
        fat_mark_dirty(f, cluster * 2 + 1);
        break;
    }
    default: {
        uint32_t old;
        memcpy(&old, f->fat + cluster * 4, 4);
        uint32_t v = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        memcpy(f->fat + cluster * 4, &v, 4);
        fat_mark_dirty(f, cluster * 4);
        fat_mark_dirty(f, cluster * 4 + 3);
        break;
    }
    }
}

static bool fat_flush(struct fat_fs *f)
{
    if (f->fat_dirty_lo > f->fat_dirty_hi)
        return true;
    uint32_t first = f->fat_dirty_lo;
    uint32_t count = f->fat_dirty_hi - f->fat_dirty_lo + 1;
    if (first + count > f->fat_sectors)
        count = f->fat_sectors - first;
    bool ok = true;
    for (uint32_t copy = 0; copy < f->fat_copies; copy++) {
        uint32_t lba = f->fat_lba + copy * f->fat_sectors + first;
        if (!blockdev_write(f->dev, lba, count, f->fat + (size_t)first * f->bytes_per_sector))
            ok = false;
    }
    f->fat_dirty_lo = 1;
    f->fat_dirty_hi = 0;
    return ok;
}

static uint32_t fat_alloc(struct fat_fs *f)
{
    uint32_t last = f->total_clusters + 2;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? f->next_free : 2;
        uint32_t end = pass == 0 ? last : f->next_free;
        for (uint32_t c = start; c < end; c++) {
            if (fat_get_raw(f, c) != 0)
                continue;
            fat_set(f, c, FAT_EOC);
            f->next_free = c + 1 < last ? c + 1 : 2;
            return c;
        }
    }
    return 0;
}

static bool zero_cluster(struct fat_fs *f, uint32_t cluster)
{
    memset(f->cluster_buf, 0, f->cluster_size);
    return blockdev_write(f->dev, cluster_lba(f, cluster), f->sectors_per_cluster, f->cluster_buf);
}

static void fat_free_chain(struct fat_fs *f, uint32_t cluster)
{
    uint32_t guard = 0;
    while (cluster >= 2 && cluster < f->total_clusters + 2 && guard++ < f->total_clusters) {
        uint32_t next = fat_get_raw(f, cluster);
        fat_set(f, cluster, 0);
        if (cluster < f->next_free)
            f->next_free = cluster;
        if (next < 2 || next >= f->total_clusters + 2)
            break;
        cluster = next;
    }
}

static uint32_t chain_extend(struct fat_fs *f, uint32_t last)
{
    uint32_t c = fat_alloc(f);
    if (!c)
        return 0;
    if (last)
        fat_set(f, last, c);
    if (!zero_cluster(f, c)) {
        fat_set(f, c, 0);
        return 0;
    }
    return c;
}

static uint32_t chain_nth(struct fat_fs *f, uint32_t first, uint32_t index, bool grow)
{
    if (first < 2) {
        if (!grow)
            return 0;
        first = chain_extend(f, 0);
        if (!first)
            return 0;
    }
    uint32_t c = first, prev = 0;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t next = fat_next(f, c);
        if (next == 0xFFFFFFFF) {
            if (!grow)
                return 0;
            prev = c;
            next = chain_extend(f, prev);
            if (!next)
                return 0;
        }
        c = next;
    }
    return c;
}

static bool dir_locate(struct fat_fs *f, uint32_t dir_ino, uint32_t offset, uint64_t *lba,
                       uint32_t *within, bool grow)
{
    *within = offset % f->bytes_per_sector;
    if (dir_ino == 0 && f->type != FAT32) {
        uint32_t sector = offset / f->bytes_per_sector;
        if (sector >= f->root_sectors)
            return false;
        *lba = f->root_lba + sector;
        return true;
    }
    uint32_t first = dir_ino ? dir_ino : f->root_cluster;
    uint32_t c = chain_nth(f, first, offset / f->cluster_size, grow);
    if (!c)
        return false;
    uint32_t sector_in_cluster = (offset % f->cluster_size) / f->bytes_per_sector;
    *lba = cluster_lba(f, c) + sector_in_cluster;
    return true;
}

static bool dir_read_entry(struct fat_fs *f, uint32_t dir_ino, uint32_t offset, uint8_t *out)
{
    uint64_t lba;
    uint32_t within;
    if (!dir_locate(f, dir_ino, offset, &lba, &within, false))
        return false;
    if (!blockdev_read(f->dev, lba, 1, f->sector_buf))
        return false;
    memcpy(out, f->sector_buf + within, 32);
    return true;
}

static bool dir_write_entry(struct fat_fs *f, uint32_t dir_ino, uint32_t offset, const uint8_t *in,
                            bool grow)
{
    uint64_t lba;
    uint32_t within;
    if (!dir_locate(f, dir_ino, offset, &lba, &within, grow))
        return false;
    if (!blockdev_read(f->dev, lba, 1, f->sector_buf))
        return false;
    memcpy(f->sector_buf + within, in, 32);
    return blockdev_write(f->dev, lba, 1, f->sector_buf);
}

static uint32_t dir_capacity(struct fat_fs *f, uint32_t dir_ino)
{
    if (dir_ino == 0 && f->type != FAT32)
        return f->root_sectors * f->bytes_per_sector;
    uint32_t first = dir_ino ? dir_ino : f->root_cluster;
    uint32_t n = 0, c = first, guard = 0;
    while (c != 0xFFFFFFFF && guard++ < f->total_clusters) {
        n += f->cluster_size;
        c = fat_next(f, c);
    }
    return n;
}

static bool dir_find_free(struct fat_fs *f, uint32_t dir_ino, uint32_t need, uint32_t *out)
{
    uint32_t capacity = dir_capacity(f, dir_ino);
    uint32_t run = 0, start = 0;
    uint8_t e[32];

    for (uint32_t off = 0; off < capacity; off += 32) {
        if (!dir_read_entry(f, dir_ino, off, e))
            return false;
        if (e[0] == 0x00 || e[0] == 0xE5) {
            if (run == 0)
                start = off;
            if (++run == need) {
                *out = start;
                return true;
            }
        } else {
            run = 0;
        }
    }

    if (dir_ino == 0 && f->type != FAT32)
        return false;

    uint32_t first = dir_ino ? dir_ino : f->root_cluster;
    uint32_t clusters = capacity / f->cluster_size;
    if (!chain_nth(f, first, clusters, true))
        return false;
    if (run == 0)
        start = capacity;
    *out = start;
    return true;
}

static void fat_now(uint16_t *date, uint16_t *time)
{
    struct rtc_time t;
    if (!rtc_read(&t)) {
        *date = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
        *time = 0;
        return;
    }
    uint16_t year = t.year > 1980 ? (uint16_t)(t.year - 1980) : 0;
    *date = (uint16_t)((year << 9) | ((t.month & 0xF) << 5) | (t.day & 0x1F));
    *time = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.minute & 0x3F) << 5) | ((t.second / 2) & 0x1F));
}

static bool short_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

struct short_probe {
    const uint8_t *want;
    bool found;
};

static bool short_probe_cb(struct filesystem *fs, const struct fat_dirent *d, const char *name,
                           uint32_t dir_offset, void *ctx)
{
    (void)fs;
    (void)name;
    (void)dir_offset;
    struct short_probe *p = ctx;
    if (memcmp(d->name, p->want, 11) == 0) {
        p->found = true;
        return true;
    }
    return false;
}

static void build_short(const char *name, uint32_t tail, uint8_t out[11])
{
    char base[9], ext[4];
    int nb = 0, ne = 0;
    const char *dot = NULL;
    for (const char *s = name; *s; s++)
        if (*s == '.')
            dot = s;

    for (const char *s = name; *s && (!dot || s < dot); s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (c == ' ' || c == '.')
            continue;
        if (!short_char_ok(c))
            c = '_';
        if (nb < 8)
            base[nb++] = c;
    }
    if (dot) {
        for (const char *s = dot + 1; *s; s++) {
            char c = *s;
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (!short_char_ok(c))
                c = '_';
            if (ne < 3)
                ext[ne++] = c;
        }
    }
    if (nb == 0)
        base[nb++] = '_';

    if (tail) {
        uint32_t hash = 2166136261u;
        for (const char *s = name; *s; s++)
            hash = (hash ^ (uint8_t)*s) * 16777619u;
        char digits[8];
        int nd = 0;
        uint32_t v = tail;
        while (v && nd < 7) {
            digits[nd++] = (char)('0' + v % 10);
            v /= 10;
        }
        if (nb > 2)
            nb = 2;
        for (int i = 3; i >= 0 && nb < 8 - nd - 1; i--)
            base[nb++] = "0123456789ABCDEF"[(hash >> (i * 4)) & 0xF];
        base[nb++] = '~';
        while (nd && nb < 8)
            base[nb++] = digits[--nd];
    }

    memset(out, ' ', 11);
    for (int i = 0; i < nb; i++)
        out[i] = (uint8_t)base[i];
    for (int i = 0; i < ne; i++)
        out[8 + i] = (uint8_t)ext[i];
}

static void make_short_name(struct filesystem *fs, const struct vfs_node *dir, const char *name,
                            uint8_t out[11])
{
    for (uint32_t tail = 0; tail < 1000; tail++) {
        build_short(name, tail, out);
        struct short_probe probe = { .want = out, .found = false };
        iterate_dir(fs, dir, short_probe_cb, &probe);
        if (!probe.found)
            return;
    }
}

static bool name_is_short(const char *name, const uint8_t sfn[11])
{
    char printed[13];
    struct fat_dirent tmp;
    memcpy(tmp.name, sfn, 11);
    short_name(&tmp, printed);
    return strcmp(printed, name) == 0;
}

static uint32_t lfn_entries_needed(const char *name, const uint8_t sfn[11])
{
    if (name_is_short(name, sfn))
        return 0;
    size_t len = strlen(name);
    return (uint32_t)((len + 12) / 13);
}

static void lfn_fill(uint8_t *e, const char *name, uint32_t seq, uint32_t total, uint8_t checksum)
{
    static const uint8_t offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    memset(e, 0, 32);
    e[0] = (uint8_t)(seq | (seq == total ? 0x40 : 0));
    e[11] = ATTR_LFN;
    e[13] = checksum;
    size_t len = strlen(name);
    size_t base = (size_t)(seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint16_t ch;
        if (base + i < len)
            ch = (uint8_t)name[base + i];
        else if (base + i == len)
            ch = 0;
        else
            ch = 0xFFFF;
        memcpy(e + offs[i], &ch, 2);
    }
}

static bool dir_add_entry(struct filesystem *fs, const struct vfs_node *dir, const char *name,
                          uint8_t attr, uint32_t cluster, uint32_t size, uint32_t *entry_off)
{
    struct fat_fs *f = fs->priv;
    uint8_t sfn[11];
    make_short_name(fs, dir, name, sfn);
    uint32_t lfns = lfn_entries_needed(name, sfn);

    uint32_t start;
    mutex_lock(&f->lock);
    if (!dir_find_free(f, (uint32_t)dir->ino, lfns + 1, &start)) {
        mutex_unlock(&f->lock);
        return false;
    }

    uint8_t checksum = short_checksum(sfn);
    bool ok = true;
    for (uint32_t i = 0; i < lfns && ok; i++) {
        uint8_t e[32];
        lfn_fill(e, name, lfns - i, lfns, checksum);
        ok = dir_write_entry(f, (uint32_t)dir->ino, start + i * 32, e, true);
    }

    struct fat_dirent d;
    memset(&d, 0, sizeof(d));
    memcpy(d.name, sfn, 11);
    d.attr = attr;
    d.cluster_lo = (uint16_t)(cluster & 0xFFFF);
    d.cluster_hi = (uint16_t)(cluster >> 16);
    d.size = size;
    uint16_t now_date, now_time;
    fat_now(&now_date, &now_time);
    d.cdate = d.mdate = d.adate = now_date;
    d.ctime = d.mtime = now_time;

    uint32_t short_off = start + lfns * 32;
    if (ok)
        ok = dir_write_entry(f, (uint32_t)dir->ino, short_off, (const uint8_t *)&d, true);
    if (ok)
        ok = fat_flush(f);
    mutex_unlock(&f->lock);
    if (ok && entry_off)
        *entry_off = short_off;
    return ok;
}

static bool dir_remove_entry(struct fat_fs *f, uint32_t dir_ino, uint32_t short_off)
{
    uint8_t e[32];
    uint32_t off = short_off;
    if (!dir_read_entry(f, dir_ino, off, e))
        return false;
    e[0] = 0xE5;
    if (!dir_write_entry(f, dir_ino, off, e, false))
        return false;

    while (off >= 32) {
        off -= 32;
        if (!dir_read_entry(f, dir_ino, off, e))
            break;
        if (e[11] != ATTR_LFN || e[0] == 0xE5)
            break;
        e[0] = 0xE5;
        if (!dir_write_entry(f, dir_ino, off, e, false))
            break;
    }
    return true;
}

static bool update_entry_size(struct fat_fs *f, struct vfs_node *node)
{
    uint8_t e[32];
    if (!dir_read_entry(f, (uint32_t)node->dir_ino, node->dir_offset, e))
        return false;
    struct fat_dirent *d = (struct fat_dirent *)e;
    d->size = (uint32_t)node->size;
    d->cluster_lo = (uint16_t)(node->ino & 0xFFFF);
    d->cluster_hi = (uint16_t)(node->ino >> 16);
    uint16_t now_date, now_time;
    fat_now(&now_date, &now_time);
    d->mdate = now_date;
    d->mtime = now_time;
    return dir_write_entry(f, (uint32_t)node->dir_ino, node->dir_offset, e, false);
}

static int64_t fat_write(struct filesystem *fs, struct vfs_node *node, uint64_t offset,
                         const void *buf, uint64_t len)
{
    struct fat_fs *f = fs->priv;
    if (!f->writable || node->is_dir)
        return -1;
    if (offset + len > 0xFFFFFFFFull)
        return -1;

    const uint8_t *in = buf;
    uint64_t done = 0;
    mutex_lock(&f->lock);

    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t index = (uint32_t)(pos / f->cluster_size);
        uint32_t within = (uint32_t)(pos % f->cluster_size);
        uint32_t first = (uint32_t)node->ino;
        uint32_t c = chain_nth(f, first, index, true);
        if (!c)
            break;
        if (first < 2) {
            uint32_t head = c;
            uint32_t walk = c;
            for (uint32_t i = 0; i < index; i++)
                walk = fat_next(f, walk);
            (void)walk;
            node->ino = head;
        }

        uint32_t chunk = f->cluster_size - within;
        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        uint64_t lba = cluster_lba(f, c);
        if (within == 0 && chunk == f->cluster_size) {
            if (!blockdev_write(f->dev, lba, f->sectors_per_cluster, in + done))
                break;
        } else {
            if (!blockdev_read(f->dev, lba, f->sectors_per_cluster, f->cluster_buf))
                break;
            memcpy(f->cluster_buf + within, in + done, chunk);
            if (!blockdev_write(f->dev, lba, f->sectors_per_cluster, f->cluster_buf))
                break;
        }
        done += chunk;
    }

    if (offset + done > node->size)
        node->size = offset + done;
    bool ok = update_entry_size(f, node);
    mutex_unlock(&f->lock);
    if (!ok && done == 0)
        return -1;
    return (int64_t)done;
}

static bool fat_truncate(struct filesystem *fs, struct vfs_node *node, uint64_t size)
{
    struct fat_fs *f = fs->priv;
    if (!f->writable || node->is_dir || size > 0xFFFFFFFFull)
        return false;

    mutex_lock(&f->lock);
    uint32_t first = (uint32_t)node->ino;
    if (size == 0) {
        if (first >= 2)
            fat_free_chain(f, first);
        node->ino = 0;
    } else if (size < node->size) {
        uint32_t keep = (uint32_t)((size + f->cluster_size - 1) / f->cluster_size);
        uint32_t c = chain_nth(f, first, keep - 1, false);
        if (c) {
            uint32_t rest = fat_next(f, c);
            fat_set(f, c, FAT_EOC);
            if (rest != 0xFFFFFFFF)
                fat_free_chain(f, rest);
        }
    }
    node->size = size;
    bool ok = update_entry_size(f, node) && fat_flush(f);
    mutex_unlock(&f->lock);
    return ok;
}

static bool fat_create(struct filesystem *fs, const struct vfs_node *dir, const char *name,
                       bool is_dir, struct vfs_node *out)
{
    struct fat_fs *f = fs->priv;
    if (!f->writable)
        return false;

    struct vfs_node existing;
    if (fat_lookup(fs, dir, name, &existing))
        return false;

    uint32_t cluster = 0;
    if (is_dir) {
        mutex_lock(&f->lock);
        cluster = chain_extend(f, 0);
        mutex_unlock(&f->lock);
        if (!cluster)
            return false;
    }

    uint32_t entry_off = 0;
    if (!dir_add_entry(fs, dir, name, is_dir ? ATTR_DIR : 0, cluster, 0, &entry_off)) {
        if (cluster) {
            mutex_lock(&f->lock);
            fat_free_chain(f, cluster);
            fat_flush(f);
            mutex_unlock(&f->lock);
        }
        return false;
    }

    if (is_dir) {
        struct vfs_node self;
        memset(&self, 0, sizeof(self));
        self.is_dir = true;
        self.ino = cluster;
        self.fs = fs;
        struct fat_dirent dot;
        uint16_t d, t;
        fat_now(&d, &t);
        mutex_lock(&f->lock);
        memset(&dot, 0, sizeof(dot));
        memset(dot.name, ' ', 11);
        dot.name[0] = '.';
        dot.attr = ATTR_DIR;
        dot.cluster_lo = (uint16_t)(cluster & 0xFFFF);
        dot.cluster_hi = (uint16_t)(cluster >> 16);
        dot.cdate = dot.mdate = dot.adate = d;
        dot.ctime = dot.mtime = t;
        dir_write_entry(f, cluster, 0, (const uint8_t *)&dot, false);
        dot.name[1] = '.';
        uint32_t parent = (uint32_t)dir->ino;
        dot.cluster_lo = (uint16_t)(parent & 0xFFFF);
        dot.cluster_hi = (uint16_t)(parent >> 16);
        dir_write_entry(f, cluster, 32, (const uint8_t *)&dot, false);
        fat_flush(f);
        mutex_unlock(&f->lock);
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->name, name, sizeof(out->name));
        out->is_dir = is_dir;
        out->size = 0;
        out->ino = cluster;
        out->dir_ino = dir->ino;
        out->dir_offset = entry_off;
        out->fs = fs;
    }
    return true;
}

struct empty_ctx {
    bool empty;
};

static bool empty_cb(const struct vfs_node *e, void *ctx)
{
    (void)e;
    ((struct empty_ctx *)ctx)->empty = false;
    return false;
}

static bool fat_unlink(struct filesystem *fs, const struct vfs_node *dir, const char *name,
                       bool is_dir)
{
    struct fat_fs *f = fs->priv;
    if (!f->writable)
        return false;

    struct vfs_node node;
    if (!fat_lookup(fs, dir, name, &node))
        return false;
    if (node.is_dir != is_dir)
        return false;

    if (is_dir) {
        struct empty_ctx e = { .empty = true };
        fat_readdir(fs, &node, empty_cb, &e);
        if (!e.empty)
            return false;
    }

    mutex_lock(&f->lock);
    if (node.ino >= 2)
        fat_free_chain(f, (uint32_t)node.ino);
    bool ok = dir_remove_entry(f, (uint32_t)dir->ino, node.dir_offset) && fat_flush(f);
    mutex_unlock(&f->lock);
    return ok;
}

static bool fat_rename(struct filesystem *fs, const struct vfs_node *dir, const char *from,
                       const char *to)
{
    struct fat_fs *f = fs->priv;
    if (!f->writable)
        return false;

    struct vfs_node node, clash;
    if (!fat_lookup(fs, dir, from, &node))
        return false;
    if (fat_lookup(fs, dir, to, &clash))
        return false;

    uint32_t entry_off = 0;
    if (!dir_add_entry(fs, dir, to, node.is_dir ? ATTR_DIR : 0, (uint32_t)node.ino,
                       (uint32_t)node.size, &entry_off))
        return false;

    mutex_lock(&f->lock);
    bool ok = dir_remove_entry(f, (uint32_t)dir->ino, node.dir_offset) && fat_flush(f);
    mutex_unlock(&f->lock);
    return ok;
}

static bool fat_sync(struct filesystem *fs)
{
    struct fat_fs *f = fs->priv;
    mutex_lock(&f->lock);
    bool ok = fat_flush(f);
    mutex_unlock(&f->lock);
    return blockdev_sync() && ok;
}

static const struct fs_ops fat_ops = {
    .root = fat_root,
    .lookup = fat_lookup,
    .read = fat_read,
    .readdir = fat_readdir,
    .write = fat_write,
    .create = fat_create,
    .unlink = fat_unlink,
    .truncate = fat_truncate,
    .rename = fat_rename,
    .sync = fat_sync,
};

struct filesystem *fat_mount(struct blockdev *dev)
{
    uint8_t *bpb = kmalloc(512);
    if (!bpb || !blockdev_read(dev, 0, 1, bpb)) {
        kfree(bpb);
        return NULL;
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        kfree(bpb);
        return NULL;
    }

    uint16_t bps, rsvd, root_entries, total16, fat16, spf16;
    uint32_t total32, spf32, root_cluster;
    uint8_t spc, nfats;
    memcpy(&bps, bpb + 11, 2);
    spc = bpb[13];
    memcpy(&rsvd, bpb + 14, 2);
    nfats = bpb[16];
    memcpy(&root_entries, bpb + 17, 2);
    memcpy(&total16, bpb + 19, 2);
    memcpy(&spf16, bpb + 22, 2);
    memcpy(&total32, bpb + 32, 4);
    memcpy(&spf32, bpb + 36, 4);
    memcpy(&root_cluster, bpb + 44, 4);
    (void)fat16;

    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || spc == 0 || (spc & (spc - 1)) || nfats == 0) {
        kfree(bpb);
        return NULL;
    }

    struct fat_fs *f = kzalloc(sizeof(*f));
    struct filesystem *fs = kzalloc(sizeof(*fs));
    if (!f || !fs) {
        kfree(f);
        kfree(fs);
        kfree(bpb);
        return NULL;
    }

    f->dev = dev;
    f->bytes_per_sector = bps;
    f->sectors_per_cluster = spc;
    f->cluster_size = bps * spc;
    f->fat_lba = rsvd;
    f->fat_sectors = spf16 ? spf16 : spf32;
    f->root_sectors = ((uint32_t)root_entries * 32 + bps - 1) / bps;
    f->root_lba = rsvd + nfats * f->fat_sectors;
    f->data_lba = f->root_lba + f->root_sectors;
    uint32_t total = total16 ? total16 : total32;
    f->total_clusters = (total - f->data_lba) / spc;
    f->root_cluster = root_cluster;

    if (f->total_clusters < 4085)
        f->type = FAT12;
    else if (f->total_clusters < 65525)
        f->type = FAT16;
    else
        f->type = FAT32;

    const uint8_t *label = f->type == FAT32 ? bpb + 71 : bpb + 43;
    memcpy(f->label, label, 11);
    f->label[11] = '\0';
    for (int i = 10; i >= 0 && f->label[i] == ' '; i--)
        f->label[i] = '\0';
    kfree(bpb);

    f->fat = kmalloc((size_t)f->fat_sectors * bps);
    f->cluster_buf = kmalloc(f->cluster_size);
    f->sector_buf = kmalloc(bps);
    if (!f->fat || !f->cluster_buf || !f->sector_buf ||
        !blockdev_read(dev, f->fat_lba, f->fat_sectors, f->fat)) {
        kfree(f->fat);
        kfree(f->cluster_buf);
        kfree(f->sector_buf);
        kfree(f);
        kfree(fs);
        return NULL;
    }
    ksnprintf(f->lock_name, sizeof(f->lock_name), "fat-%s", dev->name);
    mutex_init(&f->lock, f->lock_name);
    f->fat_copies = nfats;
    f->fat_dirty_lo = 1;
    f->fat_dirty_hi = 0;
    f->next_free = 2;
    f->writable = dev->write != NULL || (dev->parent && dev->parent->write != NULL);

    fs->type = f->type == FAT12 ? "FAT12" : f->type == FAT16 ? "FAT16" : "FAT32";
    fs->ops = &fat_ops;
    fs->priv = f;
    log_info("fat", "%s em %s: %u bytes/setor, %u setores/cluster, %u clusters, label '%s' (%s)",
             fs->type, dev->name, bps, spc, f->total_clusters, f->label,
             f->writable ? "leitura e escrita" : "somente leitura");
    return fs;
}
