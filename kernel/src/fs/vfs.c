#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/block.h>
#include <kernel/log.h>
#include <mm/heap.h>
#include <lib/string.h>

static struct filesystem *root_fs;

void vfs_init(void)
{
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_at(i);
        if (!dev->parent)
            continue;
        struct filesystem *fs = fat_mount(dev);
        if (fs) {
            root_fs = fs;
            log_info("vfs", "raiz montada: %s em %s", fs->type, dev->name);
            return;
        }
    }
    log_warn("vfs", "nenhum sistema de arquivos reconhecido");
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
    if (!root_fs)
        return false;
    struct vfs_node cur;
    if (!root_fs->ops->root(root_fs, &cur))
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
    struct vfs_node dir;
    if (!vfs_lookup(path, &dir) || !dir.is_dir)
        return false;
    return dir.fs->ops->readdir(dir.fs, &dir, cb, ctx);
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
