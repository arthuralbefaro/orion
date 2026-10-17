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
    { "tty",    file_open_tty,  size_zero,  S_IFCHR | 0620 },
    { "null",   null_open,      size_zero,  S_IFCHR | 0666 },
    { "zero",   zero_open,      size_zero,  S_IFCHR | 0666 },
    { "random", random_open,    size_zero,  S_IFCHR | 0444 },
};

struct file *blkdev_file_open(struct blockdev *dev);
uint64_t blkdev_file_size(struct blockdev *dev);

static const char *tail_of(const char *path)
{
    size_t n = strlen(DEVFS_ROOT);
    if (strncmp(path, DEVFS_ROOT, n) != 0)
        return NULL;
    if (path[n] == '\0')
        return path + n;
    if (path[n] != '/')
        return NULL;
    return path + n + 1;
}

static const struct device *find(const char *path)
{
    const char *name = tail_of(path);
    if (!name || !*name)
        return NULL;
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return NULL;
}

static struct blockdev *find_block(const char *path)
{
    const char *name = tail_of(path);
    if (!name || !*name)
        return NULL;
    return blockdev_find(name);
}

static void fill_block_node(struct blockdev *d, uint64_t ino, struct vfs_node *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, d->name, sizeof(out->name));
    out->is_dir = false;
    out->size = blkdev_file_size(d);
    out->ino = ino;
    out->fs = NULL;
}

static void fill_node(const struct device *d, uint64_t ino, struct vfs_node *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, d->name, sizeof(out->name));
    out->is_dir = false;
    out->size = d->size();
    out->ino = ino;
    out->fs = NULL;
}

bool devfs_owns(const char *path)
{
    return tail_of(path) != NULL;
}

bool devfs_is_root(const char *path)
{
    const char *tail = tail_of(path);
    return tail && *tail == '\0';
}

bool devfs_lookup(const char *path, struct vfs_node *out)
{
    if (devfs_is_root(path)) {
        memset(out, 0, sizeof(*out));
        strlcpy(out->name, "dev", sizeof(out->name));
        out->is_dir = true;
        return true;
    }
    const struct device *d = find(path);
    if (d) {
        fill_node(d, (uint64_t)(d - devices) + 1, out);
        return true;
    }
    struct blockdev *b = find_block(path);
    if (b) {
        fill_block_node(b, 100, out);
        return true;
    }
    return false;
}

uint32_t devfs_mode(const char *path)
{
    if (devfs_is_root(path))
        return S_IFDIR | 0755;
    const struct device *d = find(path);
    if (d)
        return d->mode;
    return find_block(path) ? (S_IFBLK | 0600) : 0;
}

static int devdir_readdir(struct file *f, uint64_t index, struct vfs_node *out)
{
    (void)f;
    if (index < ARRAY_SIZE(devices)) {
        fill_node(&devices[index], index + 1, out);
        return 1;
    }
    uint32_t bi = (uint32_t)(index - ARRAY_SIZE(devices));
    struct blockdev *b = blockdev_at(bi);
    if (!b)
        return 0;
    fill_block_node(b, 100 + bi, out);
    return 1;
}

static const struct file_ops devdir_ops = {
    .readdir = devdir_readdir,
};

struct file *devfs_open(const char *path)
{
    if (devfs_is_root(path)) {
        struct file *f = file_alloc(&devdir_ops, NULL);
        if (!f)
            return NULL;
        devfs_lookup(path, &f->node);
        f->has_node = true;
        f->mode = S_IFDIR | 0755;
        return f;
    }
    const struct device *d = find(path);
    if (d) {
        struct file *f = d->open();
        if (!f)
            return NULL;
        fill_node(d, (uint64_t)(d - devices) + 1, &f->node);
        f->has_node = true;
        f->mode = d->mode;
        return f;
    }
    struct blockdev *b = find_block(path);
    return b ? blkdev_file_open(b) : NULL;
}

bool devfs_readdir(vfs_dir_cb cb, void *ctx)
{
    struct vfs_node n;
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        fill_node(&devices[i], i + 1, &n);
        if (!cb(&n, ctx))
            return true;
    }
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        fill_block_node(blockdev_at(i), 100 + i, &n);
        if (!cb(&n, ctx))
            break;
    }
    return true;
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
    log_info("devfs", "%zu dispositivo(s) em %s", ARRAY_SIZE(devices), DEVFS_ROOT);
}
