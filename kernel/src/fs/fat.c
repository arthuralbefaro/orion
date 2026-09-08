#include <fs/fat.h>
#include <fs/vfs.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <mm/heap.h>
#include <lib/string.h>

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
    spinlock_t lock;
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

typedef bool (*dirent_cb)(struct filesystem *fs, const struct fat_dirent *d, const char *name, void *ctx);

static bool scan_block(struct filesystem *fs, const uint8_t *buf, uint32_t size, struct lfn_state *lfn,
                       dirent_cb cb, void *ctx, bool *stop)
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
        if (cb(fs, d, name, ctx)) {
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

    spin_lock(&f->lock);
    if (dir->ino == 0 && f->type != FAT32) {
        for (uint32_t s = 0; s < f->root_sectors && !stop; s++) {
            if (!blockdev_read(f->dev, f->root_lba + s, 1, f->cluster_buf))
                break;
            found = scan_block(fs, f->cluster_buf, f->bytes_per_sector, &lfn, cb, ctx, &stop);
        }
    } else {
        uint32_t c = dir->ino == 0 ? f->root_cluster : (uint32_t)dir->ino;
        uint32_t guard = 0;
        while (c != 0xFFFFFFFF && !stop && guard++ < f->total_clusters) {
            if (!read_cluster(f, c, f->cluster_buf))
                break;
            found = scan_block(fs, f->cluster_buf, f->cluster_size, &lfn, cb, ctx, &stop);
            c = fat_next(f, c);
        }
    }
    spin_unlock(&f->lock);
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

static bool lookup_cb(struct filesystem *fs, const struct fat_dirent *d, const char *name, void *ctx)
{
    struct lookup_ctx *l = ctx;
    if (!str_ieq(name, l->name))
        return false;
    fill_node(fs, d, name, l->out);
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
    return iterate_dir(fs, dir, lookup_cb, &ctx);
}

struct readdir_ctx {
    vfs_dir_cb cb;
    void *ctx;
};

static bool readdir_cb(struct filesystem *fs, const struct fat_dirent *d, const char *name, void *ctx)
{
    struct readdir_ctx *r = ctx;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    struct vfs_node n;
    fill_node(fs, d, name, &n);
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
    spin_lock(&f->lock);

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
    spin_unlock(&f->lock);
    return (int64_t)done;
}

static const struct fs_ops fat_ops = {
    .root = fat_root,
    .lookup = fat_lookup,
    .read = fat_read,
    .readdir = fat_readdir,
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
    if (!f->fat || !f->cluster_buf || !blockdev_read(dev, f->fat_lba, f->fat_sectors, f->fat)) {
        kfree(f->fat);
        kfree(f->cluster_buf);
        kfree(f);
        kfree(fs);
        return NULL;
    }
    spinlock_init(&f->lock);

    fs->type = f->type == FAT12 ? "FAT12" : f->type == FAT16 ? "FAT16" : "FAT32";
    fs->ops = &fat_ops;
    fs->priv = f;
    log_info("fat", "%s em %s: %u bytes/setor, %u setores/cluster, %u clusters, label '%s'",
             fs->type, dev->name, bps, spc, f->total_clusters, f->label);
    return fs;
}
