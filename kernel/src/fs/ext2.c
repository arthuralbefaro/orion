#include <fs/ext2.h>
#include <fs/vfs.h>
#include <fs/pcache.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/time.h>
#include <drivers/rtc.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

struct ext2_mnt {
    struct ext2_rs *fs;
    struct blockdev *dev;
    struct ext2_info info;
    struct mutex lock;
};

static uint32_t now_epoch(void)
{
    return (uint32_t)(rtc_boot_epoch() + time_uptime_ms() / 1000);
}

static int dev_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len, bool meta)
{
    struct blockdev *dev = ctx;
    if (!len)
        return 0;
    uint32_t ss = dev->sector_size;
    uint64_t first = off / ss, last = (off + len - 1) / ss;
    uint64_t count = last - first + 1;
    if (count > 0xFFFFFFFFull || last >= dev->sector_count)
        return -EIO;
    bool ok;
    if (off % ss == 0 && len % ss == 0) {
        ok = meta ? blockdev_read_cached(dev, first, (uint32_t)count, buf)
                  : blockdev_read(dev, first, (uint32_t)count, buf);
        return ok ? 0 : -EIO;
    }
    uint8_t *tmp = kmalloc(count * ss);
    if (!tmp)
        return -ENOMEM;
    ok = meta ? blockdev_read_cached(dev, first, (uint32_t)count, tmp)
              : blockdev_read(dev, first, (uint32_t)count, tmp);
    if (ok)
        memcpy(buf, tmp + off % ss, len);
    kfree(tmp);
    return ok ? 0 : -EIO;
}

static int dev_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len)
{
    struct blockdev *dev = ctx;
    if (!len)
        return 0;
    uint32_t ss = dev->sector_size;
    uint64_t first = off / ss, last = (off + len - 1) / ss;
    uint64_t count = last - first + 1;
    if (count > 0xFFFFFFFFull || last >= dev->sector_count)
        return -EIO;
    if (off % ss == 0 && len % ss == 0)
        return blockdev_write(dev, first, (uint32_t)count, buf) ? 0 : -EIO;
    uint8_t *tmp = kmalloc(count * ss);
    if (!tmp)
        return -ENOMEM;
    bool ok = blockdev_read(dev, first, (uint32_t)count, tmp);
    if (ok) {
        memcpy(tmp + off % ss, buf, len);
        ok = blockdev_write(dev, first, (uint32_t)count, tmp);
    }
    kfree(tmp);
    return ok ? 0 : -EIO;
}

static struct ext2_mnt *em_of(const struct vnode *v)
{
    return v->mnt->priv;
}

static int ino_vnode(struct mount *m, uint32_t ino, struct vnode **out)
{
    struct vnode *v = vcache_find(m, ino);
    if (!v) {
        struct ext2_mnt *em = m->priv;
        struct ext2_stat st;
        mutex_lock(&em->lock);
        int r = ext2_stat(em->fs, ino, &st);
        mutex_unlock(&em->lock);
        if (r < 0)
            return r;
        uint32_t type = st.mode & S_IFMT;
        if (type != S_IFDIR && type != S_IFREG && type != S_IFLNK && type != S_IFCHR && type != S_IFBLK &&
            type != S_IFIFO && type != S_IFSOCK)
            return -EIO;
        v = vnode_new(m, ino, st.mode);
        if (!v)
            return -ENOMEM;
        v->size = st.size;
        v = vcache_insert(v);
    }
    *out = v;
    return 0;
}

static int ext2v_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    if (name[0] == '.' && !name[1]) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    struct ext2_mnt *em = em_of(dir);
    uint32_t ino;
    mutex_lock(&em->lock);
    int r = ext2_lookup(em->fs, (uint32_t)dir->key, (const uint8_t *)name, (uint32_t)strlen(name), &ino);
    mutex_unlock(&em->lock);
    if (r < 0)
        return r;
    return ino_vnode(dir->mnt, ino, out);
}

struct dir_ctx {
    const struct ext2_rs *fs;
    vfs_dir_cb cb;
    void *ctx;
};

static uint32_t dtype_mode(uint8_t t)
{
    switch (t) {
    case 1: return S_IFREG;
    case 2: return S_IFDIR;
    case 3: return S_IFCHR;
    case 4: return S_IFBLK;
    case 5: return S_IFIFO;
    case 6: return S_IFSOCK;
    case 7: return S_IFLNK;
    default: return 0;
    }
}

static bool dir_cb(void *ctx, const uint8_t *name, uint32_t len, uint32_t ino, uint8_t dtype, uint64_t next)
{
    struct dir_ctx *d = ctx;
    struct vfs_dirent e;
    if (len >= sizeof(e.name))
        len = sizeof(e.name) - 1;
    memcpy(e.name, name, len);
    e.name[len] = 0;
    e.ino = ino;
    e.next = next;
    struct ext2_stat st;
    if (ext2_stat(d->fs, ino, &st) == 0) {
        e.mode = st.mode;
        e.size = st.size;
    } else {
        e.mode = dtype_mode(dtype);
        e.size = 0;
    }
    return d->cb(&e, d->ctx);
}

static int ext2v_readdir(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    struct ext2_mnt *em = em_of(dir);
    struct dir_ctx d = { .fs = em->fs, .cb = cb, .ctx = ctx };
    mutex_lock(&em->lock);
    int r = ext2_readdir(d.fs, (uint32_t)dir->key, pos, dir_cb, &d);
    mutex_unlock(&em->lock);
    return r;
}

static int ext2v_readpage(struct vnode *v, uint64_t idx, uint64_t *page)
{
    struct ext2_mnt *em = em_of(v);
    uint64_t start = idx * PAGE_SIZE;
    mutex_lock(&em->lock);
    uint64_t size = v->size;
    uint64_t expect = start >= size ? 0 : (size - start < PAGE_SIZE ? size - start : PAGE_SIZE);
    int64_t r = ext2_read(em->fs, (uint32_t)v->key, start, phys_to_virt(*page), PAGE_SIZE);
    mutex_unlock(&em->lock);
    if (r < 0)
        return (int)r;
    if ((uint64_t)r < expect)
        return -EIO;
    pcache_install(v, idx, page);
    return 0;
}

static int64_t ext2v_readlink(struct vnode *v, char *buf, uint64_t size)
{
    struct ext2_mnt *em = em_of(v);
    mutex_lock(&em->lock);
    int64_t r = ext2_readlink(em->fs, (uint32_t)v->key, (uint8_t *)buf, size);
    mutex_unlock(&em->lock);
    return r;
}

static int64_t ext2v_write(struct vnode *v, uint64_t *offp, const void *buf, uint64_t len, bool append)
{
    struct ext2_mnt *em = em_of(v);
    mutex_lock(&em->lock);
    uint64_t offset = append ? v->size : *offp;
    int64_t r = ext2_write(em->fs, (uint32_t)v->key, offset, buf, len, now_epoch());
    if (r > 0) {
        pcache_write(v, offset, buf, (uint64_t)r);
        if (offset + (uint64_t)r > v->size)
            __atomic_store_n(&v->size, offset + (uint64_t)r, __ATOMIC_RELEASE);
        *offp = offset + (uint64_t)r;
    } else if (r == 0) {
        *offp = offset;
    }
    mutex_unlock(&em->lock);
    return r;
}

static int ext2v_truncate(struct vnode *v, uint64_t size)
{
    struct ext2_mnt *em = em_of(v);
    mutex_lock(&em->lock);
    int r = ext2_truncate(em->fs, (uint32_t)v->key, size, now_epoch());
    if (r == 0) {
        pcache_truncate(v, size);
        __atomic_store_n(&v->size, size, __ATOMIC_RELEASE);
    }
    mutex_unlock(&em->lock);
    return r;
}

static int ext2v_create(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out)
{
    struct ext2_mnt *em = em_of(dir);
    uint32_t ino = 0;
    mutex_lock(&em->lock);
    int r = ext2_create(em->fs, (uint32_t)dir->key, (const uint8_t *)name, (uint32_t)strlen(name), mode,
                        now_epoch(), &ino);
    mutex_unlock(&em->lock);
    if (r < 0)
        return r;
    return ino_vnode(dir->mnt, ino, out);
}

static int ext2v_symlink(struct vnode *dir, const char *name, const char *target, struct vnode **out)
{
    struct ext2_mnt *em = em_of(dir);
    uint32_t ino = 0;
    mutex_lock(&em->lock);
    int r = ext2_symlink(em->fs, (uint32_t)dir->key, (const uint8_t *)name, (uint32_t)strlen(name),
                         (const uint8_t *)target, (uint32_t)strlen(target), now_epoch(), &ino);
    mutex_unlock(&em->lock);
    if (r < 0)
        return r;
    return ino_vnode(dir->mnt, ino, out);
}

static void forget(struct vnode *victim, uint32_t nlink)
{
    if (nlink)
        return;
    victim->unlinked = true;
    vcache_unhash(victim);
}

static int ext2v_unlink(struct vnode *dir, const char *name, struct vnode *victim)
{
    struct ext2_mnt *em = em_of(dir);
    uint32_t nlink = 1;
    mutex_lock(&em->lock);
    int r = ext2_unlink(em->fs, (uint32_t)dir->key, (const uint8_t *)name, (uint32_t)strlen(name),
                        (uint32_t)victim->key, vnode_is_dir(victim), now_epoch(), &nlink);
    if (r == 0)
        forget(victim, nlink);
    mutex_unlock(&em->lock);
    return r;
}

static int ext2v_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname,
                        struct vnode *v, struct vnode *victim)
{
    struct ext2_mnt *em = em_of(odir);
    uint32_t gone = 0, nlink = 1;
    mutex_lock(&em->lock);
    int r = ext2_rename(em->fs, (uint32_t)odir->key, (const uint8_t *)oname, (uint32_t)strlen(oname),
                        (uint32_t)ndir->key, (const uint8_t *)nname, (uint32_t)strlen(nname), (uint32_t)v->key,
                        now_epoch(), &gone, &nlink);
    if (r == 0 && victim && gone == (uint32_t)victim->key)
        forget(victim, nlink);
    mutex_unlock(&em->lock);
    return r;
}

static void ext2v_release(struct vnode *v)
{
    struct ext2_mnt *em = v->mnt->priv;
    if (!v->unlinked || !em)
        return;
    mutex_lock(&em->lock);
    int r = ext2_evict(em->fs, (uint32_t)v->key, now_epoch());
    mutex_unlock(&em->lock);
    if (r < 0)
        log_warn("ext2", "%s: falha ao liberar o inode %u (erro %d)", em->dev->name, (uint32_t)v->key, r);
}

static int ext2v_sync(struct mount *m)
{
    struct ext2_mnt *em = m->priv;
    if (!em)
        return 0;
    mutex_lock(&em->lock);
    int r = ext2_sync(em->fs, now_epoch(), false);
    mutex_unlock(&em->lock);
    return r;
}

static int ext2v_mount(struct mount *m, struct blockdev *dev, const char *data)
{
    (void)data;
    if (dev->sector_size != BLOCK_SECTOR)
        return -EINVAL;
    struct ext2_mnt *em = kzalloc(sizeof(*em));
    if (!em)
        return -ENOMEM;
    em->dev = dev;
    mutex_init(&em->lock, "ext2");
    bool want_rw = !(m->flags & MNT_RDONLY) && dev->write;
    int r = ext2_mount(dev, dev_read, want_rw ? dev_write : NULL, &em->fs, &em->info);
    if (r < 0) {
        mutex_deinit(&em->lock);
        kfree(em);
        return r;
    }
    m->priv = em;
    struct vnode *root;
    r = ino_vnode(m, ext2_root_ino(), &root);
    if (r < 0) {
        ext2_umount(em->fs);
        mutex_deinit(&em->lock);
        kfree(em);
        m->priv = NULL;
        return r;
    }
    m->root = root;
    m->subtype = "ext2";
    if (!em->info.writable)
        m->flags |= MNT_RDONLY;
    log_info("ext2", "%s: rev %u, blocos de %u bytes, %u blocos (%u livres), %u inodes, %u grupo(s), label '%s', %s",
             dev->name, em->info.rev, em->info.block_size, em->info.blocks, em->info.free_blocks,
             em->info.inodes, em->info.groups, em->info.label,
             em->info.writable ? "leitura e escrita" : "somente leitura");
    return 0;
}

static void ext2v_unmount(struct mount *m)
{
    struct ext2_mnt *em = m->priv;
    if (!em)
        return;
    mutex_lock(&em->lock);
    if (ext2_sync(em->fs, now_epoch(), true) < 0)
        log_warn("ext2", "%s: falha ao gravar o superbloco na desmontagem", em->dev->name);
    ext2_umount(em->fs);
    mutex_unlock(&em->lock);
    blockdev_sync();
    mutex_deinit(&em->lock);
    kfree(em);
    m->priv = NULL;
}

const struct fs_type ext2_fs_type = {
    .name = "ext2",
    .needs_dev = true,
    .mount = ext2v_mount,
    .unmount = ext2v_unmount,
    .sync = ext2v_sync,
    .lookup = ext2v_lookup,
    .readdir = ext2v_readdir,
    .readpage = ext2v_readpage,
    .readlink = ext2v_readlink,
    .write = ext2v_write,
    .truncate = ext2v_truncate,
    .create = ext2v_create,
    .symlink = ext2v_symlink,
    .unlink = ext2v_unlink,
    .rename = ext2v_rename,
    .release = ext2v_release,
};
