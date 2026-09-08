#include <fs/devfs.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <kernel/log.h>
#include <drivers/fbdev.h>
#include <drivers/input.h>
#include <lib/string.h>

struct device {
    const char *name;
    struct file *(*open)(void);
    uint64_t (*size)(void);
    uint32_t mode;
};

static struct file *open_fb(void)
{
    if (!fbdev_available())
        return NULL;
    return file_alloc(fbdev_ops(), NULL);
}

static uint64_t size_zero(void)
{
    return 0;
}

static const struct device devices[] = {
    { "fb0",   open_fb,        fbdev_size, S_IFCHR | 0660 },
    { "input", input_open,     size_zero,  S_IFCHR | 0640 },
    { "tty",   file_open_tty,  size_zero,  S_IFCHR | 0620 },
};

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
    if (!d)
        return false;
    fill_node(d, (uint64_t)(d - devices) + 1, out);
    return true;
}

uint32_t devfs_mode(const char *path)
{
    if (devfs_is_root(path))
        return S_IFDIR | 0755;
    const struct device *d = find(path);
    return d ? d->mode : 0;
}

static int devdir_readdir(struct file *f, uint64_t index, struct vfs_node *out)
{
    (void)f;
    if (index >= ARRAY_SIZE(devices))
        return 0;
    fill_node(&devices[index], index + 1, out);
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
    if (!d)
        return NULL;
    struct file *f = d->open();
    if (!f)
        return NULL;
    fill_node(d, (uint64_t)(d - devices) + 1, &f->node);
    f->has_node = true;
    f->mode = d->mode;
    return f;
}

bool devfs_readdir(vfs_dir_cb cb, void *ctx)
{
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        struct vfs_node n;
        fill_node(&devices[i], i + 1, &n);
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
            state = fbdev_available() ? "pronto" : "sem framebuffer";
        else if (strcmp(d->name, "input") == 0)
            state = input_grabbed() ? "em uso" : "livre";
        else if (strcmp(d->name, "tty") == 0)
            state = "pronto";
        kprintf("  %-8s %06o %10lu  %s\n", d->name, d->mode, d->size(), state);
    }
}

void devfs_init(void)
{
    input_init();
    log_info("devfs", "%zu dispositivo(s) em %s", ARRAY_SIZE(devices), DEVFS_ROOT);
}
