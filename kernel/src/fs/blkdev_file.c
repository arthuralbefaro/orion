#include <fs/block.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <lib/string.h>
#include <mm/heap.h>

static int64_t blk_read(struct file *f, void *buf, uint64_t len)
{
    struct blockdev *dev = f->priv;
    uint32_t ss = dev->sector_size;
    uint64_t total = dev->sector_count * ss;
    if (f->offset >= total)
        return 0;
    if (f->offset + len > total)
        len = total - f->offset;
    if (!len)
        return 0;

    uint64_t first = f->offset / ss;
    uint64_t last = (f->offset + len - 1) / ss;
    uint32_t count = (uint32_t)(last - first + 1);
    uint8_t *tmp = kmalloc((size_t)count * ss);
    if (!tmp)
        return -ENOMEM;
    int64_t ret = -EIO;
    if (blockdev_read(dev, first, count, tmp)) {
        memcpy(buf, tmp + (f->offset % ss), len);
        f->offset += len;
        ret = (int64_t)len;
    }
    kfree(tmp);
    return ret;
}

static int64_t blk_write(struct file *f, const void *buf, uint64_t len)
{
    struct blockdev *dev = f->priv;
    uint32_t ss = dev->sector_size;
    uint64_t total = dev->sector_count * ss;
    if (f->offset >= total)
        return -ENOSPC;
    if (f->offset + len > total)
        len = total - f->offset;
    if (!len)
        return 0;

    uint64_t first = f->offset / ss;
    uint64_t last = (f->offset + len - 1) / ss;
    uint32_t count = (uint32_t)(last - first + 1);
    uint8_t *tmp = kmalloc((size_t)count * ss);
    if (!tmp)
        return -ENOMEM;

    int64_t ret = -EIO;
    bool aligned = (f->offset % ss) == 0 && (len % ss) == 0;
    if (aligned || blockdev_read(dev, first, count, tmp)) {
        memcpy(tmp + (f->offset % ss), buf, len);
        if (blockdev_write(dev, first, count, tmp)) {
            f->offset += len;
            ret = (int64_t)len;
        }
    }
    kfree(tmp);
    return ret;
}

static const struct file_ops blk_ops = {
    .read = blk_read,
    .write = blk_write,
};

struct file *blkdev_file_open(struct blockdev *dev)
{
    struct file *f = file_alloc(&blk_ops, dev);
    if (!f)
        return NULL;
    f->mode = S_IFBLK | 0600;
    f->has_node = true;
    memset(&f->node, 0, sizeof(f->node));
    strlcpy(f->node.name, dev->name, sizeof(f->node.name));
    f->node.size = dev->sector_count * dev->sector_size;
    return f;
}

uint64_t blkdev_file_size(struct blockdev *dev)
{
    return dev->sector_count * dev->sector_size;
}
