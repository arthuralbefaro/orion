#include <fs/devfs.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <kernel/log.h>
#include <drivers/fbdev.h>
#include <drivers/input.h>
#include <fs/block.h>
#include <lib/string.h>
#include <arch/x86_64/cpu.h>
#include <lib/random.h>

struct device {
    const char *name;
    struct file *(*open)(void);
    uint64_t (*size)(void);
    uint32_t mode;
};

static uint64_t size_zero(void)
{
    return 0;
}

static int64_t null_read(struct file *f, void *buf, uint64_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return 0;
}

static int64_t null_write(struct file *f, const void *buf, uint64_t len)
{
    (void)f;
    (void)buf;
    return (int64_t)len;
}

static int64_t zero_read(struct file *f, void *buf, uint64_t len)
{
    (void)f;
    memset(buf, 0, len);
    return (int64_t)len;
}

static int64_t random_read(struct file *f, void *buf, uint64_t len)
{
    (void)f;
    random_fill(buf, len);
    return (int64_t)len;
}

static const struct file_ops null_ops = {
    .read = null_read,
    .write = null_write,
};

static const struct file_ops zero_ops = {
    .read = zero_read,
    .write = null_write,
};

static const struct file_ops random_ops = {
    .read = random_read,
    .write = null_write,
};

static struct file *null_open(void)
{
    return file_alloc(&null_ops, NULL);
}

static struct file *zero_open(void)
{
    return file_alloc(&zero_ops, NULL);
}

static struct file *random_open(void)
{
    return file_alloc(&random_ops, NULL);
}

static const struct device devices[] = {
    { "fb0",    fbdev_open,     fbdev_size, S_IFCHR | 0660 },
    { "input",  input_open,     size_zero,  S_IFCHR | 0640 },
    { "tty",    file_open_tty,  size_zero,  S_IFCHR | 0666 },
    { "null",   null_open,      size_zero,  S_IFCHR | 0666 },
    { "zero",   zero_open,      size_zero,  S_IFCHR | 0666 },
    { "random", random_open,    size_zero,  S_IFCHR | 0444 },
    { "urandom", random_open,   size_zero,  S_IFCHR | 0444 },
};

struct file *blkdev_file_open(struct blockdev *dev);
uint64_t blkdev_file_size(struct blockdev *dev);

#define DEVFS_ROOT_INO  1
#define DEVFS_BLOCK_INO 100

static int devfs_vnode(struct mount *m, uint64_t ino, uint32_t mode, uint64_t size, void *priv,
                       struct vnode **out)
{
    struct vnode *v = vcache_find(m, ino);
    if (!v) {
        v = vnode_new(m, ino, mode);
        if (!v)
            return -ENOMEM;
        v->size = size;
        v->priv = priv;
        v = vcache_insert(v);
    }
    *out = v;
    return 0;
}

static int devfs_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    if (dir->ino != DEVFS_ROOT_INO)
        return -ENOTDIR;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++)
        if (strcmp(devices[i].name, name) == 0)
            return devfs_vnode(dir->mnt, i + 2, devices[i].mode, devices[i].size(),
                               (void *)&devices[i], out);
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *b = blockdev_at(i);
        if (b && strcmp(b->name, name) == 0)
            return devfs_vnode(dir->mnt, DEVFS_BLOCK_INO + i, S_IFBLK | 0600, blkdev_file_size(b), b,
                               out);
    }
    return -ENOENT;
}

static int devfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    struct vfs_dirent e;
    (void)dir;
    uint64_t total = ARRAY_SIZE(devices) + blockdev_count();
    for (uint64_t i = *pos; i < total; i++) {
        if (i < ARRAY_SIZE(devices)) {
            strlcpy(e.name, devices[i].name, sizeof(e.name));
            e.ino = i + 2;
            e.mode = devices[i].mode;
            e.size = devices[i].size();
        } else {
            uint32_t bi = (uint32_t)(i - ARRAY_SIZE(devices));
            struct blockdev *b = blockdev_at(bi);
            if (!b)
                continue;
            strlcpy(e.name, b->name, sizeof(e.name));
            e.ino = DEVFS_BLOCK_INO + bi;
            e.mode = S_IFBLK | 0600;
            e.size = blkdev_file_size(b);
        }
        e.next = i + 1;
        if (!cb(&e, ctx))
            return 0;
        *pos = i + 1;
    }
    return 0;
}

static int devfs_open(struct vnode *v, int flags, struct file **out)
{
    (void)flags;
    if (v->ino >= DEVFS_BLOCK_INO)
        *out = blkdev_file_open(v->priv);
    else
        *out = v->priv ? ((const struct device *)v->priv)->open() : NULL;
    return *out ? 0 : -ENODEV;
}

static int devfs_mount(struct mount *m, struct blockdev *dev, const char *data)
{
    (void)dev;
    (void)data;
    struct vnode *root = vnode_new(m, DEVFS_ROOT_INO, S_IFDIR | 0755);
    if (!root)
        return -ENOMEM;
    m->root = vcache_insert(root);
    return 0;
}

const struct fs_type devfs_type = {
    .name = "devfs",
    .needs_dev = false,
    .mount = devfs_mount,
    .lookup = devfs_lookup,
    .readdir = devfs_readdir,
    .open = devfs_open,
};

struct blockdev *devfs_vnode_blockdev(const struct vnode *v)
{
    if (v->mnt->type != &devfs_type || v->ino < DEVFS_BLOCK_INO)
        return NULL;
    return v->priv;
}

void devfs_dump(void)
{
    kprintf("  %-8s %-6s %10s  %s\n", "nome", "modo", "tamanho", "estado");
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        const struct device *d = &devices[i];
        const char *state = "-";
        if (strcmp(d->name, "fb0") == 0)
            state = !fbdev_available() ? "sem framebuffer"
                    : fbdev_graphics_users() ? "modo grafico" : "pronto";
        else if (strcmp(d->name, "input") == 0)
            state = input_grabbed() ? "em uso" : "livre";
        else
            state = "pronto";
        kprintf("  %-8s %06o %10lu  %s\n", d->name, d->mode, d->size(), state);
    }
}

void devfs_init(void)
{
    input_init();
    if (!vfs_mounted())
        return;
    int r = vfs_mount_kernel("devfs", NULL, "devfs", DEVFS_ROOT);
    if (r < 0)
        log_warn("devfs", "nao foi possivel montar em %s (erro %d)", DEVFS_ROOT, -r);
    else
        log_info("devfs", "%zu dispositivo(s) em %s", ARRAY_SIZE(devices), DEVFS_ROOT);
}
