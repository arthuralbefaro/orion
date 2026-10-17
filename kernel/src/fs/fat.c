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

struct fat_node {
    uint32_t first;
    uint32_t dir_cluster;
    uint32_t dir_offset;
};

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME    0x08
#define ATTR_DIR       0x10
#define ATTR_LFN       0x0F

#define FAT_ROOT_INO 1
#define FAT_EOC      0x0FFFFFFF
#define FAT_DIR_MAX  (65536u * 32u)

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

static bool cluster_ok(struct fat_fs *f, uint32_t c)
{
    return c >= 2 && c < f->total_clusters + 2;
}

static uint32_t chain_next(struct fat_fs *f, uint32_t c)
{
    if (!cluster_ok(f, c))
        return 0xFFFFFFFF;
    uint32_t n = fat_next(f, c);
    return n == 0xFFFFFFFF || cluster_ok(f, n) ? n : 0xFFFFFFFF;
}

static uint32_t cluster_lba(struct fat_fs *fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2) * fs->sectors_per_cluster;
}

static bool read_cluster(struct fat_fs *fs, uint32_t cluster, void *buf)
{
    if (!cluster_ok(fs, cluster))
        return false;
    return blockdev_read(fs->dev, cluster_lba(fs, cluster), fs->sectors_per_cluster, buf);
}

static uint32_t ent_cluster(struct fat_fs *f, const struct fat_dirent *d)
{
    uint32_t c = d->cluster_lo;
    if (f->type == FAT32)
        c |= (uint32_t)d->cluster_hi << 16;
    return c;
}

static void set_ent_cluster(struct fat_dirent *d, uint32_t c)
{
    d->cluster_lo = (uint16_t)(c & 0xFFFF);
    d->cluster_hi = (uint16_t)(c >> 16);
}

static uint32_t norm_dir(struct fat_fs *f, uint32_t c)
{
    return (f->type == FAT32 && c == f->root_cluster) ? 0 : c;
}

static uint64_t dir_key(uint32_t c)
{
    return c ? c : FAT_ROOT_INO;
}

static uint64_t file_key(uint32_t dir, uint32_t off)
{
    return (1ull << 62) | ((uint64_t)dir << 21) | (off / 32);
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

typedef bool (*dirent_cb)(struct fat_fs *f, const struct fat_dirent *d, const char *name,
                          uint32_t dir_offset, void *ctx);

static bool scan_block(struct fat_fs *f, const uint8_t *buf, uint32_t size, struct lfn_state *lfn,
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
        if (cb(f, d, name, base_offset + off, ctx)) {
            *stop = true;
            return true;
        }
    }
    return false;
}

static bool iterate_dir(struct fat_fs *f, uint32_t dirc, dirent_cb cb, void *ctx)
{
    struct lfn_state lfn = { .valid = false };
    bool stop = false, found = false;

    if (dirc == 0 && f->type != FAT32) {
        for (uint32_t s = 0; s < f->root_sectors && !stop; s++) {
            if (!blockdev_read(f->dev, f->root_lba + s, 1, f->cluster_buf))
                break;
            found = scan_block(f, f->cluster_buf, f->bytes_per_sector, &lfn, cb, ctx, &stop,
                               s * f->bytes_per_sector);
        }
    } else {
        uint32_t c = dirc == 0 ? f->root_cluster : dirc;
        uint32_t guard = 0, base = 0;
        while (c != 0xFFFFFFFF && !stop && guard++ < f->total_clusters && base < FAT_DIR_MAX) {
            if (!read_cluster(f, c, f->cluster_buf))
                break;
            found = scan_block(f, f->cluster_buf, f->cluster_size, &lfn, cb, ctx, &stop, base);
            base += f->cluster_size;
            c = chain_next(f, c);
        }
    }
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

struct find_ctx {
    const char *name;
    struct fat_dirent d;
    uint32_t off;
    bool found;
};

static bool find_cb(struct fat_fs *f, const struct fat_dirent *d, const char *name,
                    uint32_t dir_offset, void *ctx)
{
    (void)f;
    struct find_ctx *l = ctx;
    if (!str_ieq(name, l->name))
        return false;
    l->d = *d;
    l->off = dir_offset;
    l->found = true;
    return true;
}

static bool find_entry(struct fat_fs *f, uint32_t dirc, const char *name, struct find_ctx *l)
{
    memset(l, 0, sizeof(*l));
    l->name = name;
    iterate_dir(f, dirc, find_cb, l);
    return l->found;
}

static int make_vnode(struct mount *m, uint64_t key, uint32_t mode, uint64_t size, uint32_t first,
                      uint32_t dir_cluster, uint32_t dir_offset, struct vnode **out)
{
    struct vnode *v = vcache_find(m, key);
    if (!v) {
        struct fat_node *n = kzalloc(sizeof(*n));
        if (!n)
            return -ENOMEM;
        v = vnode_new(m, key, mode);
        if (!v) {
            kfree(n);
            return -ENOMEM;
        }
        n->first = first;
        n->dir_cluster = dir_cluster;
        n->dir_offset = dir_offset;
        v->size = size;
        v->priv = n;
        v = vcache_insert(v);
    }
    *out = v;
    return 0;
}

static int vnode_for_entry(struct mount *m, struct fat_fs *f, uint32_t dirc, const struct find_ctx *l,
                           bool dotdot, struct vnode **out)
{
    uint32_t c = ent_cluster(f, &l->d);
    if (l->d.attr & ATTR_DIR) {
        c = norm_dir(f, c);
        if ((c == 0 && !dotdot) || (c != 0 && !cluster_ok(f, c)))
            return -EIO;
        return make_vnode(m, dir_key(c), S_IFDIR | 0755, 0, c, dotdot ? 0 : dirc,
                          dotdot ? 0 : l->off, out);
    }
    return make_vnode(m, file_key(dirc, l->off), S_IFREG | 0644, l->d.size, c, dirc, l->off, out);
}

static int fat_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct fat_fs *f = dir->mnt->priv;
    struct fat_node *dn = dir->priv;
    if (name[0] == '.' && !name[1]) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    bool dotdot = name[0] == '.' && name[1] == '.' && !name[2];
    if (dotdot && dir->unlinked)
        return -ENOENT;
    if (dotdot && dn->first == 0) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    mutex_lock(&f->lock);
    struct find_ctx l;
    int r = -ENOENT;
    if (!dir->unlinked && find_entry(f, dn->first, name, &l))
        r = vnode_for_entry(dir->mnt, f, dn->first, &l, dotdot, out);
    mutex_unlock(&f->lock);
    return r;
}

struct readdir_ctx {
    struct mount *m;
    uint32_t dirc;
    uint64_t *pos;
    vfs_dir_cb cb;
    void *ctx;
};

static bool readdir_cb(struct fat_fs *f, const struct fat_dirent *d, const char *name,
                       uint32_t dir_offset, void *ctx)
{
    struct readdir_ctx *r = ctx;
    if (dir_offset < *r->pos)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        *r->pos = (uint64_t)dir_offset + 32;
        return false;
    }
    struct vfs_dirent e;
    strlcpy(e.name, name, sizeof(e.name));
    e.next = (uint64_t)dir_offset + 32;
    if (d->attr & ATTR_DIR) {
        e.ino = vcache_ino(r->m, dir_key(norm_dir(f, ent_cluster(f, d))));
        e.mode = S_IFDIR | 0755;
        e.size = 0;
    } else {
        e.ino = vcache_ino(r->m, file_key(r->dirc, dir_offset));
        e.mode = S_IFREG | 0644;
        e.size = d->size;
    }
    if (!r->cb(&e, r->ctx))
        return true;
    *r->pos = e.next;
    return false;
}

static int fat_readdir(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    struct fat_fs *f = dir->mnt->priv;
    struct fat_node *dn = dir->priv;
    struct readdir_ctx r = { .m = dir->mnt, .dirc = dn->first, .pos = pos, .cb = cb, .ctx = ctx };
    mutex_lock(&f->lock);
    iterate_dir(f, dn->first, readdir_cb, &r);
    mutex_unlock(&f->lock);
    return 0;
}

static int64_t fat_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len)
{
    struct fat_fs *f = v->mnt->priv;
    struct fat_node *n = v->priv;
    mutex_lock(&f->lock);
    uint64_t size = v->size;
    if (offset >= size) {
        mutex_unlock(&f->lock);
        return 0;
    }
    if (len > size - offset)
        len = size - offset;

    uint8_t *out = buf;
    uint64_t done = 0;
    uint32_t c = cluster_ok(f, n->first) ? n->first : 0xFFFFFFFF;
    uint64_t skip = offset / f->cluster_size;
    while (skip-- && c != 0xFFFFFFFF)
        c = chain_next(f, c);
    uint64_t in_cluster = offset % f->cluster_size;

    int64_t err = 0;
    while (done < len && c != 0xFFFFFFFF) {
        if (!read_cluster(f, c, f->cluster_buf)) {
            err = -EIO;
            break;
        }
        uint64_t chunk = f->cluster_size - in_cluster;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(out + done, f->cluster_buf + in_cluster, chunk);
        done += chunk;
        in_cluster = 0;
        c = chain_next(f, c);
    }
    mutex_unlock(&f->lock);
    return done ? (int64_t)done : err;
}

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
    while (cluster_ok(f, cluster) && guard++ < f->total_clusters) {
        uint32_t next = fat_get_raw(f, cluster);
        fat_set(f, cluster, 0);
        if (cluster < f->next_free)
            f->next_free = cluster;
        if (!cluster_ok(f, next))
            break;
        cluster = next;
    }
}

static uint32_t chain_extend(struct fat_fs *f, uint32_t last)
{
    uint32_t c = fat_alloc(f);
    if (!c)
        return 0;
    if (!zero_cluster(f, c)) {
        fat_set(f, c, 0);
        return 0;
    }
    if (last)
        fat_set(f, last, c);
    return c;
}

static uint32_t chain_nth(struct fat_fs *f, uint32_t first, uint32_t index, bool grow)
{
    if (!cluster_ok(f, first))
        return 0;
    uint32_t c = first;
    for (uint32_t i = 0; i < index; i++) {
        uint32_t next = chain_next(f, c);
        if (next == 0xFFFFFFFF) {
            if (!grow)
                return 0;
            next = chain_extend(f, c);
            if (!next)
                return 0;
        }
        c = next;
    }
    return c;
}

static bool dir_locate(struct fat_fs *f, uint32_t dirc, uint32_t offset, uint64_t *lba,
                       uint32_t *within, bool grow)
{
    *within = offset % f->bytes_per_sector;
    if (dirc == 0 && f->type != FAT32) {
        uint32_t sector = offset / f->bytes_per_sector;
        if (sector >= f->root_sectors)
            return false;
        *lba = f->root_lba + sector;
        return true;
    }
    uint32_t first = dirc ? dirc : f->root_cluster;
    uint32_t c = chain_nth(f, first, offset / f->cluster_size, grow);
    if (!c)
        return false;
    uint32_t sector_in_cluster = (offset % f->cluster_size) / f->bytes_per_sector;
    *lba = cluster_lba(f, c) + sector_in_cluster;
    return true;
}

static bool dir_read_entry(struct fat_fs *f, uint32_t dirc, uint32_t offset, uint8_t *out)
{
    uint64_t lba;
    uint32_t within;
    if (!dir_locate(f, dirc, offset, &lba, &within, false))
        return false;
    if (!blockdev_read(f->dev, lba, 1, f->sector_buf))
        return false;
    memcpy(out, f->sector_buf + within, 32);
    return true;
}

static bool dir_write_entry(struct fat_fs *f, uint32_t dirc, uint32_t offset, const uint8_t *in,
                            bool grow)
{
    uint64_t lba;
    uint32_t within;
    if (!dir_locate(f, dirc, offset, &lba, &within, grow))
        return false;
    if (!blockdev_read(f->dev, lba, 1, f->sector_buf))
        return false;
    memcpy(f->sector_buf + within, in, 32);
    return blockdev_write(f->dev, lba, 1, f->sector_buf);
}

static uint32_t dir_capacity(struct fat_fs *f, uint32_t dirc)
{
    if (dirc == 0 && f->type != FAT32)
        return f->root_sectors * f->bytes_per_sector;
    uint32_t first = dirc ? dirc : f->root_cluster;
    uint32_t n = 0, c = first, guard = 0;
    while (c != 0xFFFFFFFF && guard++ < f->total_clusters) {
        n += f->cluster_size;
        c = chain_next(f, c);
    }
    return n;
}

static bool dir_find_free(struct fat_fs *f, uint32_t dirc, uint32_t need, uint32_t *out)
{
    uint32_t capacity = dir_capacity(f, dirc);
    uint32_t run = 0, start = 0;
    uint8_t e[32];

    for (uint32_t off = 0; off < capacity; off += 32) {
        if (!dir_read_entry(f, dirc, off, e))
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

    if (dirc == 0 && f->type != FAT32)
        return false;
    uint32_t at = run == 0 ? capacity : start;
    if ((uint64_t)at + (uint64_t)need * 32 > FAT_DIR_MAX)
        return false;

    uint32_t first = dirc ? dirc : f->root_cluster;
    uint32_t clusters = capacity / f->cluster_size;
    uint32_t extra = (need - run) * 32;
    uint32_t more = (extra + f->cluster_size - 1) / f->cluster_size;
    if (!chain_nth(f, first, clusters + more - 1, true))
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

static bool short_probe_cb(struct fat_fs *f, const struct fat_dirent *d, const char *name,
                           uint32_t dir_offset, void *ctx)
{
    (void)f;
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

static void make_short_name(struct fat_fs *f, uint32_t dirc, const char *name, uint8_t out[11])
{
    for (uint32_t tail = 0; tail < 1000; tail++) {
        build_short(name, tail, out);
        struct short_probe probe = { .want = out, .found = false };
        iterate_dir(f, dirc, short_probe_cb, &probe);
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

static bool name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > 255)
        return false;
    for (const char *s = name; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c >= 0x7F || strchr("\"*/:<>?\\|", c))
            return false;
    }
    return true;
}

static bool dir_add_entry(struct fat_fs *f, uint32_t dirc, const char *name,
                          const struct fat_dirent *tmpl, uint32_t *entry_off)
{
    uint8_t sfn[11];
    make_short_name(f, dirc, name, sfn);
    uint32_t lfns = lfn_entries_needed(name, sfn);

    uint32_t start;
    if (!dir_find_free(f, dirc, lfns + 1, &start))
        return false;

    uint8_t checksum = short_checksum(sfn);
    bool ok = true;
    for (uint32_t i = 0; i < lfns && ok; i++) {
        uint8_t e[32];
        lfn_fill(e, name, lfns - i, lfns, checksum);
        ok = dir_write_entry(f, dirc, start + i * 32, e, true);
    }

    struct fat_dirent d = *tmpl;
    memcpy(d.name, sfn, 11);
    uint32_t short_off = start + lfns * 32;
    if (ok)
        ok = dir_write_entry(f, dirc, short_off, (const uint8_t *)&d, true);
    if (ok && entry_off)
        *entry_off = short_off;
    return ok;
}

static bool dir_remove_entry(struct fat_fs *f, uint32_t dirc, uint32_t short_off)
{
    uint8_t e[32];
    uint32_t off = short_off;
    if (!dir_read_entry(f, dirc, off, e))
        return false;
    e[0] = 0xE5;
    if (!dir_write_entry(f, dirc, off, e, false))
        return false;

    while (off >= 32) {
        off -= 32;
        if (!dir_read_entry(f, dirc, off, e))
            break;
        if (e[11] != ATTR_LFN || e[0] == 0xE5)
            break;
        e[0] = 0xE5;
        if (!dir_write_entry(f, dirc, off, e, false))
            break;
    }
    return true;
}

#define FAT_SAVED_SLOTS 21

struct saved_entry {
    uint32_t first;
    uint32_t count;
    uint8_t slots[FAT_SAVED_SLOTS][32];
};

static bool dir_save_entry(struct fat_fs *f, uint32_t dirc, uint32_t short_off, struct saved_entry *s)
{
    if (!dir_read_entry(f, dirc, short_off, s->slots[FAT_SAVED_SLOTS - 1]))
        return false;
    uint32_t n = 1, off = short_off;
    while (off >= 32 && n < FAT_SAVED_SLOTS) {
        uint8_t e[32];
        if (!dir_read_entry(f, dirc, off - 32, e) || e[11] != ATTR_LFN || e[0] == 0xE5)
            break;
        off -= 32;
        memcpy(s->slots[FAT_SAVED_SLOTS - 1 - n], e, 32);
        n++;
    }
    s->first = off;
    s->count = n;
    return true;
}

static void dir_restore_entry(struct fat_fs *f, uint32_t dirc, const struct saved_entry *s)
{
    for (uint32_t i = 0; i < s->count; i++)
        dir_write_entry(f, dirc, s->first + i * 32, s->slots[FAT_SAVED_SLOTS - s->count + i], false);
}

static bool update_entry_size(struct fat_fs *f, struct vnode *v)
{
    struct fat_node *n = v->priv;
    if (v->unlinked)
        return true;
    uint8_t e[32];
    if (!dir_read_entry(f, n->dir_cluster, n->dir_offset, e))
        return false;
    struct fat_dirent *d = (struct fat_dirent *)e;
    d->size = (uint32_t)v->size;
    set_ent_cluster(d, n->first);
    uint16_t now_date, now_time;
    fat_now(&now_date, &now_time);
    d->mdate = now_date;
    d->mtime = now_time;
    return dir_write_entry(f, n->dir_cluster, n->dir_offset, e, false);
}

static int64_t write_locked(struct fat_fs *f, struct vnode *v, uint64_t offset, const uint8_t *in,
                            uint64_t len)
{
    struct fat_node *n = v->priv;
    uint64_t done = 0;
    int64_t err = 0;
    uint32_t cur = 0, cur_index = 0;

    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t index = (uint32_t)(pos / f->cluster_size);
        uint32_t within = (uint32_t)(pos % f->cluster_size);
        if (!cluster_ok(f, n->first)) {
            uint32_t head = chain_extend(f, 0);
            if (!head) {
                err = -ENOSPC;
                break;
            }
            n->first = head;
            cur = head;
            cur_index = 0;
        }
        uint32_t c;
        if (cur && index == cur_index) {
            c = cur;
        } else if (cur && index == cur_index + 1) {
            c = chain_next(f, cur);
            if (c == 0xFFFFFFFF)
                c = chain_extend(f, cur);
        } else {
            c = chain_nth(f, n->first, index, true);
        }
        if (!c) {
            err = -ENOSPC;
            break;
        }
        cur = c;
        cur_index = index;

        uint32_t chunk = f->cluster_size - within;
        if (chunk > len - done)
            chunk = (uint32_t)(len - done);

        uint64_t lba = cluster_lba(f, c);
        bool ok;
        if (within == 0 && chunk == f->cluster_size) {
            if (in) {
                ok = blockdev_write(f->dev, lba, f->sectors_per_cluster, in + done);
            } else {
                memset(f->cluster_buf, 0, f->cluster_size);
                ok = blockdev_write(f->dev, lba, f->sectors_per_cluster, f->cluster_buf);
            }
        } else {
            ok = blockdev_read(f->dev, lba, f->sectors_per_cluster, f->cluster_buf);
            if (ok) {
                if (in)
                    memcpy(f->cluster_buf + within, in + done, chunk);
                else
                    memset(f->cluster_buf + within, 0, chunk);
                ok = blockdev_write(f->dev, lba, f->sectors_per_cluster, f->cluster_buf);
            }
        }
        if (!ok) {
            err = -EIO;
            break;
        }
        done += chunk;
    }

    if (offset + done > v->size)
        v->size = offset + done;
    return done ? (int64_t)done : err;
}

static int64_t fat_write(struct vnode *v, uint64_t *offp, const void *buf, uint64_t len, bool append)
{
    struct fat_fs *f = v->mnt->priv;
    if (!f->writable)
        return -EROFS;

    mutex_lock(&f->lock);
    uint64_t offset = append ? v->size : *offp;
    if (offset > 0xFFFFFFFFull || len > 0xFFFFFFFFull - offset) {
        mutex_unlock(&f->lock);
        return -EFBIG;
    }
    if (len == 0) {
        *offp = offset;
        mutex_unlock(&f->lock);
        return 0;
    }
    int64_t r = 0;
    if (offset > v->size) {
        uint64_t gap = offset - v->size;
        int64_t z = write_locked(f, v, v->size, NULL, gap);
        if (z < (int64_t)gap)
            r = z < 0 ? z : -ENOSPC;
    }
    if (r == 0)
        r = write_locked(f, v, offset, buf, len);
    *offp = offset + (r > 0 ? (uint64_t)r : 0);
    bool ok = update_entry_size(f, v);
    mutex_unlock(&f->lock);
    if (!ok && r >= 0)
        return -EIO;
    return r;
}

static int fat_truncate(struct vnode *v, uint64_t size)
{
    struct fat_fs *f = v->mnt->priv;
    struct fat_node *n = v->priv;
    if (!f->writable)
        return -EROFS;
    if (size > 0xFFFFFFFFull)
        return -EFBIG;

    mutex_lock(&f->lock);
    int r = 0;
    if (size > v->size) {
        uint64_t gap = size - v->size;
        int64_t w = write_locked(f, v, v->size, NULL, gap);
        if (w < (int64_t)gap)
            r = w < 0 ? (int)w : -ENOSPC;
    } else if (size == 0) {
        if (cluster_ok(f, n->first))
            fat_free_chain(f, n->first);
        n->first = 0;
        v->size = 0;
    } else if (size < v->size) {
        uint32_t keep = (uint32_t)((size + f->cluster_size - 1) / f->cluster_size);
        uint32_t c = chain_nth(f, n->first, keep - 1, false);
        if (c) {
            uint32_t rest = chain_next(f, c);
            fat_set(f, c, FAT_EOC);
            if (rest != 0xFFFFFFFF)
                fat_free_chain(f, rest);
        }
        v->size = size;
    }
    if (!update_entry_size(f, v) && r == 0)
        r = -EIO;
    if (!fat_flush(f) && r == 0)
        r = -EIO;
    mutex_unlock(&f->lock);
    return r;
}

static void dirent_now(struct fat_dirent *d, uint8_t attr, uint32_t cluster)
{
    memset(d, 0, sizeof(*d));
    memset(d->name, ' ', 11);
    d->attr = attr;
    set_ent_cluster(d, cluster);
    uint16_t now_date, now_time;
    fat_now(&now_date, &now_time);
    d->cdate = d->mdate = d->adate = now_date;
    d->ctime = d->mtime = now_time;
}

static bool nonempty_cb(struct fat_fs *f, const struct fat_dirent *d, const char *name,
                        uint32_t dir_offset, void *ctx)
{
    (void)f;
    (void)d;
    (void)dir_offset;
    (void)ctx;
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int fat_create(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out)
{
    struct fat_fs *f = dir->mnt->priv;
    struct fat_node *dn = dir->priv;
    if (!f->writable)
        return -EROFS;
    if (!name_ok(name))
        return -EINVAL;
    bool is_dir = (mode & S_IFMT) == S_IFDIR;

    mutex_lock(&f->lock);
    struct find_ctx l;
    if (find_entry(f, dn->first, name, &l)) {
        mutex_unlock(&f->lock);
        return -EEXIST;
    }

    uint32_t cluster = 0;
    if (is_dir) {
        cluster = chain_extend(f, 0);
        if (!cluster) {
            mutex_unlock(&f->lock);
            return -ENOSPC;
        }
        struct fat_dirent dot;
        dirent_now(&dot, ATTR_DIR, cluster);
        dot.name[0] = '.';
        bool ok = dir_write_entry(f, cluster, 0, (const uint8_t *)&dot, false);
        dot.name[1] = '.';
        set_ent_cluster(&dot, dn->first);
        ok = ok && dir_write_entry(f, cluster, 32, (const uint8_t *)&dot, false);
        if (!ok) {
            fat_free_chain(f, cluster);
            fat_flush(f);
            mutex_unlock(&f->lock);
            return -EIO;
        }
    }

    struct fat_dirent d;
    dirent_now(&d, is_dir ? ATTR_DIR : 0, cluster);
    uint32_t off = 0;
    if (!dir_add_entry(f, dn->first, name, &d, &off)) {
        if (cluster)
            fat_free_chain(f, cluster);
        fat_flush(f);
        mutex_unlock(&f->lock);
        return -ENOSPC;
    }
    bool flushed = fat_flush(f);
    int r = is_dir ? make_vnode(dir->mnt, dir_key(cluster), S_IFDIR | 0755, 0, cluster, dn->first, off, out)
                   : make_vnode(dir->mnt, file_key(dn->first, off), S_IFREG | 0644, 0, 0, dn->first, off, out);
    mutex_unlock(&f->lock);
    if (r == 0 && !flushed)
        log_warn("fat", "create: falha ao gravar a FAT de %s", f->dev->name);
    return r;
}

static int fat_unlink(struct vnode *dir, const char *name, struct vnode *victim)
{
    struct fat_fs *f = dir->mnt->priv;
    struct fat_node *dn = dir->priv;
    struct fat_node *vn = victim->priv;
    if (!f->writable)
        return -EROFS;

    mutex_lock(&f->lock);
    struct find_ctx l;
    int r = 0;
    if (!find_entry(f, dn->first, name, &l))
        r = -ENOENT;
    else if (vnode_is_dir(victim) && iterate_dir(f, vn->first, nonempty_cb, NULL))
        r = -ENOTEMPTY;
    else if (!dir_remove_entry(f, dn->first, l.off))
        r = -EIO;
    if (r == 0) {
        victim->unlinked = true;
        vcache_unhash(victim);
        if (!fat_flush(f))
            r = -EIO;
    }
    mutex_unlock(&f->lock);
    return r;
}

static int fat_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname,
                      struct vnode *v, struct vnode *victim)
{
    struct fat_fs *f = odir->mnt->priv;
    struct fat_node *odn = odir->priv;
    struct fat_node *ndn = ndir->priv;
    struct fat_node *n = v->priv;
    if (!f->writable)
        return -EROFS;
    if (!name_ok(nname))
        return -EINVAL;

    mutex_lock(&f->lock);
    struct find_ctx a, b;
    int r = 0;
    bool replace = false, self_rename = false;
    if (!find_entry(f, odn->first, oname, &a)) {
        r = -ENOENT;
    } else if (find_entry(f, ndn->first, nname, &b)) {
        self_rename = odn->first == ndn->first && b.off == a.off;
        if (!self_rename && !victim)
            r = -EEXIST;
        else if (!self_rename && vnode_is_dir(victim) &&
                 iterate_dir(f, ((struct fat_node *)victim->priv)->first, nonempty_cb, NULL))
            r = -ENOTEMPTY;
        else if (!self_rename)
            replace = true;
    }
    struct fat_dirent d = a.d;
    if (r == 0 && !vnode_is_dir(v)) {
        d.size = (uint32_t)v->size;
        set_ent_cluster(&d, n->first);
    }
    bool moved_dir = vnode_is_dir(v) && odn->first != ndn->first;
    uint8_t dotdot[32], dotdot_old[32];
    if (r == 0 && moved_dir &&
        (!dir_read_entry(f, n->first, 32, dotdot) || dotdot[0] != '.' || dotdot[1] != '.'))
        r = -EIO;
    struct saved_entry *src = r == 0 ? kmalloc(sizeof(*src) * 2) : NULL;
    struct saved_entry *vic = src ? src + 1 : NULL;
    if (r == 0 && (!src || !dir_save_entry(f, odn->first, a.off, src)))
        r = src ? -EIO : -ENOMEM;
    if (r == 0 && replace && !dir_save_entry(f, ndn->first, b.off, vic))
        r = -EIO;
    bool dotdot_written = false;
    if (r == 0 && moved_dir) {
        memcpy(dotdot_old, dotdot, 32);
        set_ent_cluster((struct fat_dirent *)dotdot, ndn->first);
        if (dir_write_entry(f, n->first, 32, dotdot, false))
            dotdot_written = true;
        else
            r = -EIO;
    }
    uint32_t noff = 0;
    bool added = false, src_gone = false, victim_gone = false;
    if (r == 0) {
        added = dir_add_entry(f, ndn->first, nname, &d, &noff);
        if (!added && replace)
            victim_gone = dir_remove_entry(f, ndn->first, b.off);
        else if (!added && self_rename)
            src_gone = dir_remove_entry(f, odn->first, a.off);
        if (!added && (victim_gone || src_gone))
            added = dir_add_entry(f, ndn->first, nname, &d, &noff);
        if (!added)
            r = -ENOSPC;
    }
    if (r == 0 && !src_gone) {
        src_gone = dir_remove_entry(f, odn->first, a.off);
        if (!src_gone)
            r = -EIO;
    }
    if (r == 0 && replace && !victim_gone) {
        victim_gone = dir_remove_entry(f, ndn->first, b.off);
        if (!victim_gone)
            r = -EIO;
    }
    if (r == 0 && replace) {
        victim->unlinked = true;
        vcache_unhash(victim);
    }
    if (r < 0) {
        if (added)
            dir_remove_entry(f, ndn->first, noff);
        if (src_gone)
            dir_restore_entry(f, odn->first, src);
        if (victim_gone)
            dir_restore_entry(f, ndn->first, vic);
        if (dotdot_written)
            dir_write_entry(f, n->first, 32, dotdot_old, false);
    }
    kfree(src);
    if (r == 0) {
        n->dir_cluster = ndn->first;
        n->dir_offset = noff;
        if (!vnode_is_dir(v))
            vcache_rekey(v, file_key(ndn->first, noff));
        if (!fat_flush(f))
            r = -EIO;
    }
    mutex_unlock(&f->lock);
    return r;
}

static void fat_release(struct vnode *v)
{
    struct fat_node *n = v->priv;
    if (!n)
        return;
    if (v->unlinked) {
        struct fat_fs *f = v->mnt->priv;
        mutex_lock(&f->lock);
        if (cluster_ok(f, n->first))
            fat_free_chain(f, n->first);
        fat_flush(f);
        mutex_unlock(&f->lock);
    }
    kfree(n);
}

static int fat_sync(struct mount *m)
{
    struct fat_fs *f = m->priv;
    mutex_lock(&f->lock);
    bool ok = fat_flush(f);
    mutex_unlock(&f->lock);
    return blockdev_sync() && ok ? 0 : -EIO;
}

static void fat_free(struct fat_fs *f)
{
    kfree(f->fat);
    kfree(f->cluster_buf);
    kfree(f->sector_buf);
    kfree(f);
}

static void fat_unmount(struct mount *m)
{
    struct fat_fs *f = m->priv;
    if (!f)
        return;
    mutex_deinit(&f->lock);
    fat_free(f);
    m->priv = NULL;
}

static int fat_mount(struct mount *m, struct blockdev *dev, const char *data)
{
    (void)data;
    uint8_t *bpb = kmalloc(512);
    if (!bpb)
        return -ENOMEM;
    if (!blockdev_read(dev, 0, 1, bpb)) {
        kfree(bpb);
        return -EIO;
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        kfree(bpb);
        return -EINVAL;
    }

    uint16_t bps, rsvd, root_entries, total16, spf16;
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

    uint32_t fat_sectors = spf16 ? spf16 : spf32;
    uint32_t total = total16 ? total16 : total32;
    if (bps != dev->sector_size || spc == 0 || (spc & (spc - 1)) || nfats == 0 || nfats > 4 ||
        rsvd == 0 || fat_sectors == 0) {
        kfree(bpb);
        return -EINVAL;
    }

    struct fat_fs *f = kzalloc(sizeof(*f));
    if (!f) {
        kfree(bpb);
        return -ENOMEM;
    }

    f->dev = dev;
    f->bytes_per_sector = bps;
    f->sectors_per_cluster = spc;
    f->cluster_size = bps * spc;
    f->fat_lba = rsvd;
    f->fat_sectors = fat_sectors;
    f->root_sectors = ((uint32_t)root_entries * 32 + bps - 1) / bps;
    f->root_lba = rsvd + nfats * f->fat_sectors;
    f->data_lba = f->root_lba + f->root_sectors;
    f->root_cluster = root_cluster;
    if (total <= f->data_lba + spc) {
        kfree(f);
        kfree(bpb);
        return -EINVAL;
    }
    f->total_clusters = (total - f->data_lba) / spc;

    if (f->total_clusters < 4085)
        f->type = FAT12;
    else if (f->total_clusters < 65525)
        f->type = FAT16;
    else
        f->type = FAT32;

    uint64_t tc = (uint64_t)f->total_clusters + 2;
    uint64_t need = f->type == FAT12 ? (tc - 1) + (tc - 1) / 2 + 2 : f->type == FAT16 ? tc * 2 : tc * 4;
    if ((f->type == FAT32 && (!cluster_ok(f, root_cluster) || f->total_clusters > 0x0FFFFFF5)) ||
        need > (uint64_t)fat_sectors * bps || (uint64_t)total > dev->sector_count) {
        kfree(f);
        kfree(bpb);
        return -EINVAL;
    }

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
        fat_free(f);
        return -EIO;
    }
    ksnprintf(f->lock_name, sizeof(f->lock_name), "fat-%s", dev->name);
    mutex_init(&f->lock, f->lock_name);
    f->fat_copies = nfats;
    f->fat_dirty_lo = 1;
    f->fat_dirty_hi = 0;
    f->next_free = 2;
    f->writable = dev->write != NULL || (dev->parent && dev->parent->write != NULL);

    struct fat_node *rn = kzalloc(sizeof(*rn));
    struct vnode *root = rn ? vnode_new(m, FAT_ROOT_INO, S_IFDIR | 0755) : NULL;
    if (!root) {
        kfree(rn);
        mutex_deinit(&f->lock);
        fat_free(f);
        return -ENOMEM;
    }
    root->priv = rn;
    m->priv = f;
    m->root = vcache_insert(root);
    m->subtype = f->type == FAT12 ? "FAT12" : f->type == FAT16 ? "FAT16" : "FAT32";
    if (!f->writable)
        m->flags |= MNT_RDONLY;
    log_info("fat", "%s em %s: %u bytes/setor, %u setores/cluster, %u clusters, label '%s' (%s)",
             m->subtype, dev->name, bps, spc, f->total_clusters, f->label,
             f->writable ? "leitura e escrita" : "somente leitura");
    return 0;
}

const struct fs_type fat_fs_type = {
    .name = "vfat",
    .needs_dev = true,
    .mount = fat_mount,
    .unmount = fat_unmount,
    .sync = fat_sync,
    .lookup = fat_lookup,
    .readdir = fat_readdir,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .create = fat_create,
    .unlink = fat_unlink,
    .rename = fat_rename,
    .release = fat_release,
};
