#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/devfs.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <mm/heap.h>
#include <lib/string.h>

static struct filesystem *root_fs;
static struct filesystem *mnt_fs;
static struct blockdev *root_dev;
static char mnt_dev[BLOCK_NAME_MAX];

bool vfs_mount_extra(void)
{
    if (mnt_fs)
        return true;
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_at(i);
        if (dev == root_dev || (root_dev && dev == root_dev->parent))
            continue;
        if (root_dev && dev->parent == root_dev->parent && dev->parent)
            continue;
        struct filesystem *fs = fat_mount(dev);
        if (!fs)
            continue;
        mnt_fs = fs;
        strlcpy(mnt_dev, dev->name, sizeof(mnt_dev));
        log_info("vfs", "/mnt montado: %s em %s", fs->type, dev->name);
        return true;
    }
    return false;
}

const char *vfs_mnt_device(void)
{
    return mnt_fs ? mnt_dev : NULL;
}

void vfs_init(void)
{
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_at(i);
        if (!dev->parent)
            continue;
        struct filesystem *fs = fat_mount(dev);
        if (fs) {
            root_fs = fs;
            root_dev = dev;
            log_info("vfs", "raiz montada: %s em %s", fs->type, dev->name);
            break;
        }
    }
    if (!root_fs)
        log_warn("vfs", "nenhum sistema de arquivos reconhecido");
    vfs_mount_extra();
}

static bool mnt_path(const char *path, const char **rest)
{
    if (strncmp(path, "/mnt", 4) != 0)
        return false;
    if (path[4] == 0) {
        *rest = "/";
        return true;
    }
    if (path[4] != '/')
        return false;
    *rest = path + 4;
    return true;
}

bool vfs_mounted(void)
{
    return root_fs != NULL;
}

const char *vfs_root_type(void)
{
    return root_fs ? root_fs->type : "-";
}

bool vfs_lookup(const char *path, struct vfs_node *out)
{
    if (devfs_owns(path))
        return devfs_lookup(path, out);

    struct filesystem *fs = root_fs;
    const char *rest;
    if (mnt_path(path, &rest)) {
        if (!mnt_fs)
            return false;
        fs = mnt_fs;
        path = rest;
    }
    if (!fs)
        return false;
    struct vfs_node cur;
    if (!fs->ops->root(fs, &cur))
        return false;

    const char *p = path;
    while (*p == '/')
        p++;

    while (*p) {
        char part[VFS_NAME_MAX];
        size_t n = 0;
        while (*p && *p != '/' && n < VFS_NAME_MAX - 1)
            part[n++] = *p++;
        part[n] = '\0';
        while (*p == '/')
            p++;
        if (n == 0)
            continue;
        if (!cur.is_dir)
            return false;
        struct vfs_node next;
        if (!cur.fs->ops->lookup(cur.fs, &cur, part, &next))
            return false;
        cur = next;
    }
    *out = cur;
    return true;
}

int64_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buf, uint64_t len)
{
    if (node->is_dir)
        return -1;
    return node->fs->ops->read(node->fs, node, offset, buf, len);
}

bool vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx)
{
    if (devfs_is_root(path))
        return devfs_readdir(cb, ctx);
    if (devfs_owns(path))
        return false;
    struct vfs_node dir;
    if (!vfs_lookup(path, &dir) || !dir.is_dir)
        return false;
    return dir.fs->ops->readdir(dir.fs, &dir, cb, ctx);
}

static struct filesystem *fs_for(const char *path, const char **rest)
{
    if (mnt_path(path, rest))
        return mnt_fs;
    *rest = path;
    return root_fs;
}

static bool split_path(const char *path, char *dir, size_t dir_size, const char **base)
{
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash)
        return false;
    size_t n = (size_t)(slash - path);
    if (n == 0)
        n = 1;
    if (n >= dir_size)
        return false;
    memcpy(dir, path, n);
    dir[n] = 0;
    *base = slash + 1;
    return **base != 0;
}

bool vfs_writable(void)
{
    return root_fs && root_fs->ops->write != NULL;
}

int64_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buf, uint64_t len)
{
    if (!node->fs || !node->fs->ops->write)
        return -1;
    return node->fs->ops->write(node->fs, node, offset, buf, len);
}

bool vfs_truncate(struct vfs_node *node, uint64_t size)
{
    if (!node->fs || !node->fs->ops->truncate)
        return false;
    return node->fs->ops->truncate(node->fs, node, size);
}

bool vfs_create(const char *path, bool is_dir, struct vfs_node *out)
{
    char dir[VFS_PATH_MAX];
    const char *base, *rest;
    struct filesystem *fs = fs_for(path, &rest);
    if (!fs || !fs->ops->create || !split_path(path, dir, sizeof(dir), &base))
        return false;
    struct vfs_node parent;
    if (!vfs_lookup(dir, &parent) || !parent.is_dir)
        return false;
    return fs->ops->create(fs, &parent, base, is_dir, out);
}

bool vfs_unlink(const char *path, bool is_dir)
{
    char dir[VFS_PATH_MAX];
    const char *base, *rest;
    struct filesystem *fs = fs_for(path, &rest);
    if (!fs || !fs->ops->unlink || !split_path(path, dir, sizeof(dir), &base))
        return false;
    struct vfs_node parent;
    if (!vfs_lookup(dir, &parent) || !parent.is_dir)
        return false;
    return fs->ops->unlink(fs, &parent, base, is_dir);
}

bool vfs_rename(const char *from, const char *to)
{
    char dfrom[VFS_PATH_MAX], dto[VFS_PATH_MAX];
    const char *bfrom, *bto, *rest;
    struct filesystem *fs = fs_for(from, &rest);
    if (!fs || !fs->ops->rename)
        return false;
    if (!split_path(from, dfrom, sizeof(dfrom), &bfrom) ||
        !split_path(to, dto, sizeof(dto), &bto))
        return false;
    if (strcmp(dfrom, dto) != 0)
        return false;
    struct vfs_node parent;
    if (!vfs_lookup(dfrom, &parent) || !parent.is_dir)
        return false;
    return fs->ops->rename(fs, &parent, bfrom, bto);
}

bool vfs_sync(void)
{
    bool ok = true;
    if (root_fs && root_fs->ops->sync)
        ok = root_fs->ops->sync(root_fs);
    if (mnt_fs && mnt_fs->ops->sync)
        ok = mnt_fs->ops->sync(mnt_fs) && ok;
    return blockdev_sync() && ok;
}

static void bdflush(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(2000);
        vfs_sync();
    }
}

void vfs_flush_init(void)
{
    thread_create("bdflush", bdflush, NULL);
    log_info("vfs", "bdflush: descarrega o cache a cada 2 s");
}

void *vfs_read_all(const char *path, uint64_t *size_out)
{
    struct vfs_node node;
    if (!vfs_lookup(path, &node) || node.is_dir)
        return NULL;
    void *buf = kmalloc(node.size ? node.size : 1);
    if (!buf)
        return NULL;
    uint64_t done = 0;
    while (done < node.size) {
        int64_t n = vfs_read(&node, done, (uint8_t *)buf + done, node.size - done);
        if (n <= 0) {
            kfree(buf);
            return NULL;
        }
        done += (uint64_t)n;
    }
    if (size_out)
        *size_out = node.size;
    return buf;
}
