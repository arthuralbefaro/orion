#include <fs/initramfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <boot/bootinfo.h>
#include <kernel/log.h>
#include <kernel/syscall.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <lib/string.h>

#define CPIO_HEADER   110
#define CPIO_NAME_MAX 256

struct cpio_entry {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t mtime;
    uint64_t size;
    const char *name;
    const uint8_t *data;
};

static uint64_t hex8(const uint8_t *p, bool *ok)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint64_t)(c - 'A' + 10);
        else
            *ok = false;
    }
    return v;
}

static uint64_t align4(uint64_t x)
{
    return (x + 3) & ~3ull;
}

static int next_entry(const uint8_t *base, uint64_t size, uint64_t *pos, struct cpio_entry *e)
{
    uint64_t p = *pos;
    if (p > size || size - p < CPIO_HEADER || memcmp(base + p, "070701", 6) != 0)
        return -EINVAL;
    const uint8_t *h = base + p + 6;
    bool ok = true;
    e->mode = (uint32_t)hex8(h + 8, &ok);
    e->uid = (uint32_t)hex8(h + 16, &ok);
    e->gid = (uint32_t)hex8(h + 24, &ok);
    e->mtime = hex8(h + 40, &ok);
    e->size = hex8(h + 48, &ok);
    uint64_t namesize = hex8(h + 88, &ok);
    if (!ok || namesize == 0 || namesize > CPIO_NAME_MAX)
        return -EINVAL;
    uint64_t name_at = p + CPIO_HEADER;
    if (namesize > size - name_at || base[name_at + namesize - 1] != 0)
        return -EINVAL;
    uint64_t data_at = align4(name_at + namesize);
    if (data_at > size || e->size > size - data_at)
        return -EINVAL;
    e->name = (const char *)base + name_at;
    e->data = base + data_at;
    *pos = align4(data_at + e->size);
    return 0;
}

static int unpack_one(struct vnode *root, const struct cpio_entry *e)
{
    const char *path = e->name;
    while (*path == '/' || (path[0] == '.' && path[1] == '/'))
        path += path[0] == '/' ? 1 : 2;
    if (!*path || strcmp(path, ".") == 0)
        return 0;
    uint32_t type = e->mode & S_IFMT;
    struct vnode *v = NULL;
    int r;
    if (type == S_IFDIR) {
        r = vfs_mkdir(root, path, e->mode & 07777);
        if (r == 0 || r == -EEXIST)
            r = vfs_walk_ex(root, path, false, &v, NULL);
    } else if (type == S_IFREG) {
        r = vfs_mknod(root, path, S_IFREG | (e->mode & 07777), &v);
        if (r == 0 && e->size) {
            uint64_t off = 0;
            int64_t w = vfs_write(v, &off, e->data, e->size, false);
            if (w != (int64_t)e->size)
                r = w < 0 ? (int)w : -EIO;
        }
    } else if (type == S_IFLNK) {
        char target[CPIO_NAME_MAX];
        if (e->size == 0 || e->size >= sizeof(target))
            return -ENAMETOOLONG;
        memcpy(target, e->data, e->size);
        target[e->size] = 0;
        r = vfs_symlink(root, path, target);
        if (r == 0)
            r = vfs_walk_ex(root, path, false, &v, NULL);
    } else {
        return 0;
    }
    if (r < 0) {
        if (v)
            vnode_put(v);
        return r;
    }
    struct vattr va = { .mask = VA_UID | VA_GID | VA_MTIME, .uid = e->uid, .gid = e->gid };
    if (type != S_IFLNK) {
        va.mask |= VA_MODE;
        va.mode = e->mode & 07777;
    }
    va.mtime.sec = (int64_t)e->mtime;
    r = vfs_setattr(v, &va);
    vnode_put(v);
    return r;
}

static uint64_t give_back(const struct boot_module *m)
{
    uint64_t start = (uint64_t)m->address - hhdm_offset;
    uint64_t first = PAGE_ALIGN_UP(start);
    uint64_t last = (start + m->size) & ~(uint64_t)(PAGE_SIZE - 1);
    if (last <= first)
        return 0;
    return pmm_add_region(first, (last - first) / PAGE_SIZE);
}

bool initramfs_mount_root(void)
{
    struct boot_module *m = (struct boot_module *)boot_find_module("initramfs");
    if (!m || !m->address || m->size < CPIO_HEADER)
        return false;
    int r = vfs_mount(&tmpfs_type, NULL, "initramfs", NULL, "/", 0, NULL);
    if (r < 0) {
        log_warn("initramfs", "tmpfs da raiz nao montou (erro %d)", -r);
        return false;
    }
    struct vnode *root = vfs_root();
    const uint8_t *base = m->address;
    uint64_t pos = 0, entries = 0, bytes = 0, failed = 0;
    struct cpio_entry e;
    while ((r = next_entry(base, m->size, &pos, &e)) == 0) {
        if (strcmp(e.name, "TRAILER!!!") == 0)
            break;
        int u = unpack_one(root, &e);
        if (u < 0) {
            if (failed++ < 8)
                log_warn("initramfs", "%s: erro %d", e.name, -u);
            continue;
        }
        entries++;
        bytes += e.size;
    }
    vnode_put(root);
    if (r < 0)
        log_warn("initramfs", "cpio corrompido na posicao %lu; o que veio antes ficou", pos);
    uint64_t pages = give_back(m);
    m->address = NULL;
    m->size = 0;
    log_info("vfs", "raiz montada: tmpfs do initramfs (%lu entradas, %lu KiB, %lu falha(s)); %lu KiB do modulo "
             "devolvidos", entries, bytes / 1024, failed, pages * PAGE_SIZE / 1024);
    return true;
}
