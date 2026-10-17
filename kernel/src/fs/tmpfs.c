#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/memory.h>
#include <lib/string.h>

#define RADIX_SHIFT 9
#define RADIX_SIZE  512u
#define RADIX_MASK  (RADIX_SIZE - 1)

struct tnode {
    uint64_t ino;
    uint32_t mode;
    uint64_t size;
    char *name;
    struct tnode *parent;
    struct tnode *next;
    struct tnode *children;
    struct tnode *last;
    uint64_t seq;
    uint64_t next_seq;
    uint64_t top;
    uint8_t height;
    uint32_t vrefs;
    bool linked;
};

struct tmpfs {
    struct mutex lock;
    struct tnode *root;
    uint64_t next_ino;
    uint64_t max_pages;
    uint64_t used_pages;
    uint64_t max_inodes;
    uint64_t nr_inodes;
};

static bool is_dir(const struct tnode *n)
{
    return (n->mode & S_IFMT) == S_IFDIR;
}

static uint64_t *table(uint64_t phys)
{
    return phys_to_virt(phys);
}

static bool take_page(struct tmpfs *t, uint64_t *slot)
{
    if (t->used_pages >= t->max_pages)
        return false;
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t reserve = st.total_pages / 16;
    if (reserve < TMPFS_RESERVE_MIN)
        reserve = TMPFS_RESERVE_MIN;
    if (st.free_pages <= reserve)
        return false;
    uint64_t p = pmm_alloc_page();
    if (!p)
        return false;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    *slot = p;
    t->used_pages++;
    return true;
}

static void drop_page(struct tmpfs *t, uint64_t *slot)
{
    pmm_free_page(*slot);
    *slot = 0;
    t->used_pages--;
}

static uint64_t cover(uint8_t height)
{
    return 1ull << (RADIX_SHIFT * height);
}

static uint64_t *page_slot(struct tmpfs *t, struct tnode *n, uint64_t idx, bool create)
{
    if (!n->top) {
        if (!create)
            return NULL;
        uint8_t h = 0;
        while (idx >= cover(h))
            h++;
        n->height = h;
        if (h == 0)
            return &n->top;
        if (!take_page(t, &n->top))
            return NULL;
    }
    while (idx >= cover(n->height)) {
        if (!create || n->height >= TMPFS_RADIX_LEVELS)
            return NULL;
        uint64_t grown;
        if (!take_page(t, &grown))
            return NULL;
        table(grown)[0] = n->top;
        n->top = grown;
        n->height++;
    }
    uint64_t *slot = &n->top;
    for (uint8_t lvl = n->height; lvl > 0; lvl--) {
        slot = &table(*slot)[(idx >> (RADIX_SHIFT * (lvl - 1))) & RADIX_MASK];
        if (lvl > 1 && !*slot && !(create && take_page(t, slot)))
            return NULL;
    }
    return slot;
}

static void free_level(struct tmpfs *t, uint64_t *slot, uint8_t lvl, uint64_t base, uint64_t first)
{
    if (!*slot)
        return;
    if (lvl > 0) {
        if (base + cover(lvl) <= first)
            return;
        uint64_t *tb = table(*slot);
        for (uint64_t i = 0; i < RADIX_SIZE; i++)
            free_level(t, &tb[i], (uint8_t)(lvl - 1), base + i * cover((uint8_t)(lvl - 1)), first);
    }
    if (base >= first)
        drop_page(t, slot);
}

static void free_from(struct tmpfs *t, struct tnode *n, uint64_t first)
{
    free_level(t, &n->top, n->height, 0, first);
    if (!n->top)
        n->height = 0;
}

static void free_node(struct tmpfs *t, struct tnode *n)
{
    free_from(t, n, 0);
    t->nr_inodes--;
    kfree(n->name);
    kfree(n);
}

static void maybe_free(struct tmpfs *t, struct tnode *n)
{
    if (!n->linked && n->vrefs == 0)
        free_node(t, n);
}

static char *dup_name(const char *name)
{
    size_t len = strlen(name) + 1;
    char *s = kmalloc(len);
    if (s)
        memcpy(s, name, len);
    return s;
}

static struct tnode *new_node(struct tmpfs *t, const char *name, uint32_t mode)
{
    if (t->nr_inodes >= t->max_inodes)
        return NULL;
    struct tnode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    n->name = dup_name(name);
    if (!n->name) {
        kfree(n);
        return NULL;
    }
    n->ino = t->next_ino++;
    n->mode = mode;
    t->nr_inodes++;
    return n;
}

static void link_child(struct tnode *dir, struct tnode *n)
{
    n->parent = dir;
    n->next = NULL;
    n->seq = dir->next_seq++;
    if (dir->last)
        dir->last->next = n;
    else
        dir->children = n;
    dir->last = n;
    n->linked = true;
}

static void unlink_child(struct tnode *dir, struct tnode *n)
{
    struct tnode *prev = NULL;
    for (struct tnode *c = dir->children; c; prev = c, c = c->next) {
        if (c != n)
            continue;
        if (prev)
            prev->next = c->next;
        else
            dir->children = c->next;
        if (dir->last == c)
            dir->last = prev;
        break;
    }
    n->next = NULL;
    n->linked = false;
}

static struct tnode *find_child(struct tnode *dir, const char *name)
{
    for (struct tnode *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static int node_vnode(struct mount *m, struct tnode *n, struct vnode **out)
{
    struct vnode *v = vcache_find(m, n->ino);
    if (!v) {
        v = vnode_new(m, n->ino, n->mode);
        if (!v)
            return -ENOMEM;
        v->size = n->size;
        v->priv = n;
        n->vrefs++;
        v = vcache_insert(v);
    }
    *out = v;
    return 0;
}

static int tmpfs_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct tmpfs *t = dir->mnt->priv;
    struct tnode *d = dir->priv;
    if (name[0] == '.' && !name[1]) {
        vnode_get(dir);
        *out = dir;
        return 0;
    }
    mutex_lock(&t->lock);
    int r = -ENOENT;
    if (d->linked) {
        struct tnode *n = name[0] == '.' && name[1] == '.' && !name[2] ? d->parent : find_child(d, name);
        if (n)
            r = node_vnode(dir->mnt, n, out);
    }
    mutex_unlock(&t->lock);
    return r;
}

static int tmpfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    struct tmpfs *t = dir->mnt->priv;
    struct tnode *d = dir->priv;
    struct vfs_dirent e;
    mutex_lock(&t->lock);
    for (struct tnode *c = d->children; c; c = c->next) {
        if (c->seq < *pos)
            continue;
        strlcpy(e.name, c->name, sizeof(e.name));
        e.ino = c->ino;
        e.mode = c->mode;
        e.size = c->size;
        e.next = c->seq + 1;
        if (!cb(&e, ctx))
            break;
        *pos = c->seq + 1;
    }
    mutex_unlock(&t->lock);
    return 0;
}

static int64_t tmpfs_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len)
{
    struct tmpfs *t = v->mnt->priv;
    struct tnode *n = v->priv;
    mutex_lock(&t->lock);
    if (offset >= n->size) {
        mutex_unlock(&t->lock);
        return 0;
    }
    if (len > n->size - offset)
        len = n->size - offset;
    uint8_t *out = buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t within = pos % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t *slot = page_slot(t, n, pos / PAGE_SIZE, false);
        if (slot && *slot)
            memcpy(out + done, (uint8_t *)phys_to_virt(*slot) + within, chunk);
        else
            memset(out + done, 0, chunk);
        done += chunk;
    }
    mutex_unlock(&t->lock);
    return (int64_t)done;
}

static void set_size(struct vnode *v, struct tnode *n, uint64_t size)
{
    n->size = size;
    v->size = size;
}

static int64_t write_locked(struct tmpfs *t, struct tnode *n, uint64_t offset, const uint8_t *in,
                            uint64_t len)
{
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint64_t within = pos % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t *slot = page_slot(t, n, pos / PAGE_SIZE, true);
        if (!slot || (!*slot && !take_page(t, slot)))
            break;
        memcpy((uint8_t *)phys_to_virt(*slot) + within, in + done, chunk);
        done += chunk;
    }
    return done ? (int64_t)done : -ENOSPC;
}

static int64_t tmpfs_write(struct vnode *v, uint64_t *offp, const void *buf, uint64_t len, bool append)
{
    struct tmpfs *t = v->mnt->priv;
    struct tnode *n = v->priv;
    mutex_lock(&t->lock);
    uint64_t offset = append ? n->size : *offp;
    if (len && offset >= TMPFS_MAX_FILE) {
        mutex_unlock(&t->lock);
        return -EFBIG;
    }
    if (len > TMPFS_MAX_FILE - offset)
        len = TMPFS_MAX_FILE - offset;
    int64_t r = len ? write_locked(t, n, offset, buf, len) : 0;
    if (r > 0 && offset + (uint64_t)r > n->size)
        set_size(v, n, offset + (uint64_t)r);
    if (r < (int64_t)len)
        free_from(t, n, (n->size + PAGE_SIZE - 1) / PAGE_SIZE);
    *offp = offset + (r > 0 ? (uint64_t)r : 0);
    mutex_unlock(&t->lock);
    return r;
}

static int tmpfs_truncate(struct vnode *v, uint64_t size)
{
    struct tmpfs *t = v->mnt->priv;
    struct tnode *n = v->priv;
    if (size > TMPFS_MAX_FILE)
        return -EFBIG;
    mutex_lock(&t->lock);
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    free_from(t, n, keep);
    if (size < n->size) {
        uint64_t within = size % PAGE_SIZE;
        uint64_t *slot = within ? page_slot(t, n, keep - 1, false) : NULL;
        if (slot && *slot)
            memset((uint8_t *)phys_to_virt(*slot) + within, 0, PAGE_SIZE - within);
    }
    set_size(v, n, size);
    mutex_unlock(&t->lock);
    return 0;
}

static bool name_ok(const char *name)
{
    size_t len = strlen(name);
    return len > 0 && len < VFS_NAME_MAX && !strchr(name, '/');
}

static int tmpfs_create(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out)
{
    struct tmpfs *t = dir->mnt->priv;
    struct tnode *d = dir->priv;
    if (!name_ok(name))
        return -EINVAL;
    mutex_lock(&t->lock);
    int r = 0;
    if (!d->linked)
        r = -ENOENT;
    else if (find_child(d, name))
        r = -EEXIST;
    struct tnode *n = NULL;
    if (r == 0) {
        n = new_node(t, name, (mode & S_IFMT) == S_IFDIR ? (S_IFDIR | 0755) : (S_IFREG | 0644));
        if (!n)
            r = -ENOSPC;
    }
    if (r == 0) {
        link_child(d, n);
        r = node_vnode(dir->mnt, n, out);
        if (r < 0) {
            unlink_child(d, n);
            maybe_free(t, n);
        }
    }
    mutex_unlock(&t->lock);
    return r;
}

static int tmpfs_unlink(struct vnode *dir, const char *name, struct vnode *victim)
{
    struct tmpfs *t = dir->mnt->priv;
    struct tnode *d = dir->priv;
    mutex_lock(&t->lock);
    struct tnode *n = find_child(d, name);
    int r = 0;
    if (!n || n != victim->priv)
        r = -ENOENT;
    else if (is_dir(n) && n->children)
        r = -ENOTEMPTY;
    if (r == 0) {
        unlink_child(d, n);
        victim->unlinked = true;
        vcache_unhash(victim);
    }
    mutex_unlock(&t->lock);
    return r;
}

static int tmpfs_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname,
                        struct vnode *v, struct vnode *victim)
{
    struct tmpfs *t = odir->mnt->priv;
    struct tnode *od = odir->priv, *nd = ndir->priv;
    struct tnode *n = v->priv;
    if (!name_ok(nname))
        return -EINVAL;
    mutex_lock(&t->lock);
    int r = 0;
    struct tnode *clash = find_child(nd, nname);
    if (find_child(od, oname) != n)
        r = -ENOENT;
    else if (clash && clash != n && (!victim || clash != victim->priv))
        r = -EEXIST;
    else if (clash && clash != n && is_dir(clash) && clash->children)
        r = -ENOTEMPTY;
    char *name = r == 0 ? dup_name(nname) : NULL;
    if (r == 0 && !name)
        r = -ENOMEM;
    if (r == 0) {
        if (clash && clash != n) {
            unlink_child(nd, clash);
            victim->unlinked = true;
            vcache_unhash(victim);
        }
        kfree(n->name);
        n->name = name;
        if (od != nd) {
            unlink_child(od, n);
            link_child(nd, n);
        }
    }
    mutex_unlock(&t->lock);
    return r;
}

static void tmpfs_release(struct vnode *v)
{
    struct tmpfs *t = v->mnt->priv;
    struct tnode *n = v->priv;
    if (!n)
        return;
    mutex_lock(&t->lock);
    n->vrefs--;
    if (n != t->root)
        maybe_free(t, n);
    mutex_unlock(&t->lock);
}

static void free_tree(struct tmpfs *t)
{
    struct tnode *n = t->root;
    for (;;) {
        if (n->children) {
            n = n->children;
            continue;
        }
        if (n == t->root) {
            free_node(t, n);
            return;
        }
        struct tnode *p = n->parent;
        p->children = n->next;
        if (p->last == n)
            p->last = NULL;
        free_node(t, n);
        n = p;
    }
}

static void tmpfs_unmount(struct mount *m)
{
    struct tmpfs *t = m->priv;
    if (!t)
        return;
    free_tree(t);
    mutex_deinit(&t->lock);
    kfree(t);
    m->priv = NULL;
}

static int parse_number(const char **sp, uint64_t *out)
{
    const char *s = *sp;
    uint64_t v = 0;
    if (*s < '0' || *s > '9')
        return -EINVAL;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s++ - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -EINVAL;
        v = v * 10 + d;
    }
    uint64_t mult = 1;
    if (*s == 'k' || *s == 'K')
        mult = 1024;
    else if (*s == 'm' || *s == 'M')
        mult = 1024 * 1024;
    else if (*s == 'g' || *s == 'G')
        mult = 1024ull * 1024 * 1024;
    if (mult > 1) {
        if (v > UINT64_MAX / mult)
            return -EINVAL;
        v *= mult;
        s++;
    }
    *sp = s;
    *out = v;
    return 0;
}

static int parse_opts(const char *data, uint64_t total_pages, uint64_t *max_pages, uint64_t *max_inodes)
{
    const char *s = data ? data : "";
    while (*s) {
        uint64_t v;
        if (strncmp(s, "size=", 5) == 0) {
            s += 5;
            if (parse_number(&s, &v) < 0)
                return -EINVAL;
            if (*s == '%') {
                s++;
                if (v > 100)
                    return -EINVAL;
                v = total_pages * v / 100;
            } else {
                v = v / PAGE_SIZE + (v % PAGE_SIZE != 0);
            }
            *max_pages = v ? v : total_pages;
        } else if (strncmp(s, "nr_inodes=", 10) == 0) {
            s += 10;
            if (parse_number(&s, &v) < 0)
                return -EINVAL;
            *max_inodes = v ? v : UINT64_MAX;
        } else if (strncmp(s, "mode=", 5) == 0 || strncmp(s, "uid=", 4) == 0 || strncmp(s, "gid=", 4) == 0) {
            while (*s && *s != ',')
                s++;
        } else {
            return -EINVAL;
        }
        if (*s == ',')
            s++;
        else if (*s)
            return -EINVAL;
    }
    return 0;
}

static int tmpfs_mount(struct mount *m, struct blockdev *dev, const char *data)
{
    (void)dev;
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t max_pages = st.total_pages / 2, max_inodes = st.total_pages / 2;
    if (parse_opts(data, st.total_pages, &max_pages, &max_inodes) < 0)
        return -EINVAL;
    struct tmpfs *t = kzalloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->max_pages = max_pages;
    t->max_inodes = max_inodes;
    t->next_ino = 1;
    t->root = new_node(t, "/", S_IFDIR | 01777);
    struct vnode *root = t->root ? vnode_new(m, t->root->ino, t->root->mode) : NULL;
    if (!root) {
        if (t->root)
            free_node(t, t->root);
        kfree(t);
        return -ENOMEM;
    }
    t->root->parent = t->root;
    t->root->linked = true;
    t->root->vrefs = 1;
    root->priv = t->root;
    mutex_init(&t->lock, "tmpfs");
    m->priv = t;
    m->root = vcache_insert(root);
    return 0;
}

const struct fs_type tmpfs_type = {
    .name = "tmpfs",
    .needs_dev = false,
    .mount = tmpfs_mount,
    .unmount = tmpfs_unmount,
    .lookup = tmpfs_lookup,
    .readdir = tmpfs_readdir,
    .read = tmpfs_read,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .create = tmpfs_create,
    .unlink = tmpfs_unlink,
    .rename = tmpfs_rename,
    .release = tmpfs_release,
};

uint64_t tmpfs_used_pages(struct mount *m)
{
    struct tmpfs *t = m->priv;
    return t ? t->used_pages : 0;
}

void tmpfs_init(void)
{
    static const char *const points[] = { "/tmp", "/run" };
    if (!vfs_mounted())
        return;
    for (size_t i = 0; i < ARRAY_SIZE(points); i++) {
        int r = vfs_mount_kernel("tmpfs", NULL, "tmpfs", points[i]);
        if (r < 0)
            log_warn("tmpfs", "nao foi possivel montar em %s (erro %d)", points[i], -r);
        else
            log_info("tmpfs", "%s montado (ate metade da RAM)", points[i]);
    }
}
