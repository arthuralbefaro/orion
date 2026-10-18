#include <fs/vfs.h>
#include <fs/fat.h>
#include <fs/devfs.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <fs/ext2.h>
#include <fs/block.h>
#include <kernel/file.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/timesys.h>
#include <drivers/rtc.h>
#include <mm/heap.h>
#include <lib/string.h>

#define VCACHE_BUCKETS 256
#define VFS_MAX_INACTIVE 512

#define WALK_NOFOLLOW 1u

struct nameidata {
    struct vnode *dir;
    char last[VFS_NAME_MAX];
    bool slash;
    bool followed;
};

struct walk {
    struct vnode *cur;
    const char *p;
    char *buf;
    char *tgt;
    int links;
    bool followed;
};

static spinlock_t vfs_lock;
static struct vnode *vcache[VCACHE_BUCKETS];
static struct mutex mount_lock;
static struct mount *mounts;
static struct mount *root_mount;
static uint32_t next_mount_id = 1;
static struct vnode *lru_head;
static struct vnode *lru_tail;
static uint32_t inactive_count;
static uint64_t inactive_pages;

static const struct fs_type *const fs_types[] = { &fat_fs_type, &devfs_type, &tmpfs_type, &procfs_type, &ext2_fs_type };

const struct fs_type *vfs_find_type(const char *name)
{
    if (strcmp(name, "fat") == 0 || strcmp(name, "msdos") == 0)
        name = "vfat";
    if (strcmp(name, "devtmpfs") == 0)
        name = "devfs";
    if (strcmp(name, "procfs") == 0)
        name = "proc";
    for (size_t i = 0; i < ARRAY_SIZE(fs_types); i++)
        if (strcmp(fs_types[i]->name, name) == 0)
            return fs_types[i];
    return NULL;
}

static uint32_t bucket(const struct mount *m, uint64_t key)
{
    uint64_t h = (key ^ ((uint64_t)(uintptr_t)m >> 4)) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) % VCACHE_BUCKETS;
}

static bool ino_displaced_locked(struct mount *m, uint64_t ino)
{
    for (struct vnode *d = m->displaced; d; d = d->dnext)
        if (d->ino == ino)
            return true;
    return false;
}

static void displace_locked(struct vnode *v)
{
    if (v->displaced)
        return;
    v->displaced = true;
    v->dnext = v->mnt->displaced;
    v->mnt->displaced = v;
}

static void undisplace_locked(struct vnode *v)
{
    if (!v->displaced)
        return;
    struct vnode **pp = &v->mnt->displaced;
    while (*pp && *pp != v)
        pp = &(*pp)->dnext;
    if (*pp)
        *pp = v->dnext;
    v->dnext = NULL;
    v->displaced = false;
}

static uint64_t alt_ino_locked(struct mount *m, uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    for (uint64_t i = 0;; i++) {
        uint64_t ino = (1ull << 63) | ((h + i) & ~(1ull << 63));
        if (!ino_displaced_locked(m, ino))
            return ino;
    }
}

struct vnode *vnode_new(struct mount *m, uint64_t key, uint32_t mode)
{
    struct vnode *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    v->mnt = m;
    v->key = key;
    v->ino = key;
    v->mode = mode;
    v->refs = 1;
    spinlock_init(&v->pc.lock);
    mutex_init_private(&v->io_lock, "vnode-io");
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    m->nvnodes++;
    if (ino_displaced_locked(m, key))
        v->ino = alt_ino_locked(m, key);
    spin_unlock_irqrestore(&vfs_lock, fl);
    return v;
}

static struct vnode *find_locked(struct mount *m, uint64_t key)
{
    for (struct vnode *v = vcache[bucket(m, key)]; v; v = v->hnext)
        if (v->mnt == m && v->key == key)
            return v;
    return NULL;
}

uint64_t vcache_ino(struct mount *m, uint64_t key)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    struct vnode *v = find_locked(m, key);
    uint64_t ino = key;
    if (v)
        ino = v->ino;
    else if (ino_displaced_locked(m, key))
        ino = alt_ino_locked(m, key);
    spin_unlock_irqrestore(&vfs_lock, fl);
    return ino;
}

static void hash_locked(struct vnode *v)
{
    uint32_t b = bucket(v->mnt, v->key);
    v->hnext = vcache[b];
    vcache[b] = v;
    v->hashed = true;
}

static void unhash_locked(struct vnode *v)
{
    if (!v->hashed)
        return;
    struct vnode **pp = &vcache[bucket(v->mnt, v->key)];
    while (*pp && *pp != v)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = v->hnext;
    v->hnext = NULL;
    v->hashed = false;
}

static void lru_add_locked(struct vnode *v)
{
    v->inactive = true;
    v->lru_prev = NULL;
    v->lru_next = lru_head;
    if (lru_head)
        lru_head->lru_prev = v;
    lru_head = v;
    if (!lru_tail)
        lru_tail = v;
    inactive_count++;
    v->lru_pages = v->pc.npages;
    inactive_pages += v->lru_pages;
}

static void lru_remove_locked(struct vnode *v)
{
    if (!v->inactive)
        return;
    if (v->lru_prev)
        v->lru_prev->lru_next = v->lru_next;
    else
        lru_head = v->lru_next;
    if (v->lru_next)
        v->lru_next->lru_prev = v->lru_prev;
    else
        lru_tail = v->lru_prev;
    v->lru_prev = v->lru_next = NULL;
    v->inactive = false;
    inactive_count--;
    inactive_pages -= v->lru_pages;
    v->lru_pages = 0;
}

uint64_t vfs_cache_reclaimable(void)
{
    return __atomic_load_n(&inactive_pages, __ATOMIC_RELAXED);
}

static void free_vnode(struct vnode *v)
{
    struct mount *m = v->mnt;
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    undisplace_locked(v);
    spin_unlock_irqrestore(&vfs_lock, fl);
    pcache_drop(v);
    if (m->type->release)
        m->type->release(v);
    fl = spin_lock_irqsave(&vfs_lock);
    if (v->evicting)
        m->evicting--;
    m->nvnodes--;
    spin_unlock_irqrestore(&vfs_lock, fl);
    kfree(v);
}

struct vnode *vcache_find(struct mount *m, uint64_t key)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    struct vnode *v = find_locked(m, key);
    if (v) {
        lru_remove_locked(v);
        v->refs++;
    }
    spin_unlock_irqrestore(&vfs_lock, fl);
    return v;
}

struct vnode *vcache_insert(struct vnode *v)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    struct vnode *old = find_locked(v->mnt, v->key);
    if (old) {
        lru_remove_locked(old);
        old->refs++;
        spin_unlock_irqrestore(&vfs_lock, fl);
        free_vnode(v);
        return old;
    }
    hash_locked(v);
    spin_unlock_irqrestore(&vfs_lock, fl);
    return v;
}

void vcache_rekey(struct vnode *v, uint64_t key)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    bool was = v->hashed;
    unhash_locked(v);
    v->key = key;
    if (was)
        hash_locked(v);
    if (v->ino != key)
        displace_locked(v);
    spin_unlock_irqrestore(&vfs_lock, fl);
}

void vcache_unhash(struct vnode *v)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    unhash_locked(v);
    displace_locked(v);
    spin_unlock_irqrestore(&vfs_lock, fl);
}

void vnode_get(struct vnode *v)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    v->refs++;
    spin_unlock_irqrestore(&vfs_lock, fl);
}

void vnode_put(struct vnode *v)
{
    if (!v)
        return;
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    bool last = --v->refs == 0;
    bool keep = last && v->hashed && !v->unlinked && v->pc.npages > 0;
    if (keep)
        lru_add_locked(v);
    else if (last)
        unhash_locked(v);
    bool shrink = inactive_count > VFS_MAX_INACTIVE ||
                  (keep && pcache_over_budget() && inactive_pages > v->lru_pages);
    spin_unlock_irqrestore(&vfs_lock, fl);
    if (last && !keep)
        free_vnode(v);
    if (shrink)
        vfs_cache_shrink();
}

static void evict_locked(struct vnode *v)
{
    lru_remove_locked(v);
    unhash_locked(v);
    v->evicting = true;
    v->mnt->evicting++;
}

void vfs_cache_shrink(void)
{
    uint64_t total = pcache_total_pages(), budget = pcache_budget_pages();
    uint64_t want = total > budget ? total - budget + 1 : 0, freed = 0;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        struct vnode *v = lru_tail;
        if (!v || (inactive_count <= VFS_MAX_INACTIVE && freed >= want)) {
            spin_unlock_irqrestore(&vfs_lock, fl);
            return;
        }
        freed += v->lru_pages;
        evict_locked(v);
        spin_unlock_irqrestore(&vfs_lock, fl);
        free_vnode(v);
    }
}

uint64_t vfs_cache_reclaim(uint64_t want)
{
    uint64_t freed = 0;
    while (freed < want) {
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        struct vnode *v = lru_tail;
        if (v) {
            freed += v->lru_pages;
            evict_locked(v);
        }
        spin_unlock_irqrestore(&vfs_lock, fl);
        if (!v)
            break;
        free_vnode(v);
    }
    return freed;
}

static void evict_mount(struct mount *m)
{
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        struct vnode *v = lru_head;
        while (v && v->mnt != m)
            v = v->lru_next;
        if (v)
            evict_locked(v);
        bool busy = m->evicting > 0;
        spin_unlock_irqrestore(&vfs_lock, fl);
        if (v) {
            free_vnode(v);
            continue;
        }
        if (!busy)
            return;
        thread_sleep_ms(1);
    }
}

struct vnode *vfs_root(void)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    struct vnode *r = root_mount ? root_mount->root : NULL;
    if (r)
        r->refs++;
    spin_unlock_irqrestore(&vfs_lock, fl);
    return r;
}

bool vfs_mounted(void)
{
    return root_mount != NULL;
}

const char *vfs_root_type(void)
{
    return root_mount ? root_mount->subtype : "-";
}

bool vfs_is_rdonly(const struct vnode *v)
{
    return (v->mnt->flags & MNT_RDONLY) != 0;
}

void vfs_now(struct vfs_time *t)
{
    timesys_realtime(&t->sec, &t->nsec);
}

void vfs_getattr(struct vnode *v, struct vattr *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = v->mode;
    out->nlink = vnode_is_dir(v) ? 2 : 1;
    out->blocks = (__atomic_load_n(&v->size, __ATOMIC_ACQUIRE) + 511) / 512;
    out->atime.sec = out->mtime.sec = out->ctime.sec = (int64_t)rtc_boot_epoch();
    if (v->mnt->type->getattr)
        v->mnt->type->getattr(v, out);
}

uint32_t vattr_apply_mode(uint32_t old, const struct vattr *in)
{
    uint32_t m = old;
    if (in->mask & VA_MODE)
        m = (old & S_IFMT) | (in->mode & 07777);
    if ((in->mask & VA_KILL_SUID) && (m & S_IFMT) != S_IFDIR) {
        m &= ~(uint32_t)S_ISUID;
        if (m & S_IXGRP)
            m &= ~(uint32_t)S_ISGID;
    }
    return m;
}

int vfs_setattr(struct vnode *v, const struct vattr *in)
{
    if (vfs_is_rdonly(v))
        return -EROFS;
    if (!v->mnt->type->setattr)
        return -EPERM;
    return v->mnt->type->setattr(v, in);
}

static struct vnode *follow_mounts(struct vnode *v)
{
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    while (v->covered_by) {
        struct vnode *r = v->covered_by->root;
        r->refs++;
        v->refs--;
        v = r;
    }
    spin_unlock_irqrestore(&vfs_lock, fl);
    return v;
}

static int step_dotdot(struct vnode **cur)
{
    struct vnode *v = *cur;
    uint64_t fl = spin_lock_irqsave(&vfs_lock);
    while (v != root_mount->root && v == v->mnt->root && v->mnt->covered) {
        struct vnode *c = v->mnt->covered;
        c->refs++;
        v->refs--;
        v = c;
    }
    spin_unlock_irqrestore(&vfs_lock, fl);
    *cur = v;
    if (v == v->mnt->root)
        return 0;
    if (v->unlinked)
        return -ENOENT;
    struct vnode *up = NULL;
    int r = v->mnt->type->lookup(v, "..", &up);
    if (r < 0)
        return r;
    vnode_put(v);
    *cur = follow_mounts(up);
    return 0;
}

static bool is_dot(const char *s)
{
    return s[0] == '.' && (!s[1] || (s[1] == '.' && !s[2]));
}

static int step(struct vnode **cur, const char *name)
{
    struct vnode *v = *cur;
    if (!vnode_is_dir(v))
        return -ENOTDIR;
    if (name[0] == '.' && !name[1])
        return 0;
    if (name[0] == '.' && name[1] == '.' && !name[2])
        return step_dotdot(cur);
    if (v->unlinked)
        return -ENOENT;
    struct vnode *next = NULL;
    int r = v->mnt->type->lookup(v, name, &next);
    if (r < 0)
        return r;
    vnode_put(v);
    *cur = follow_mounts(next);
    return 0;
}

static struct vnode *start_of(struct vnode *start, const char *path)
{
    if (path[0] == '/' || !start)
        return vfs_root();
    vnode_get(start);
    return start;
}

static int next_component(const char **pp, char *name)
{
    const char *p = *pp;
    while (*p == '/')
        p++;
    const char *s = p;
    while (*p && *p != '/')
        p++;
    size_t n = (size_t)(p - s);
    *pp = p;
    if (n >= VFS_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, s, n);
    name[n] = '\0';
    return (int)n;
}

static bool only_slashes(const char *p)
{
    while (*p == '/')
        p++;
    return *p == '\0';
}

static int strlcat_slash(char *buf, size_t size)
{
    size_t n = strlen(buf);
    if (n + 2 > size)
        return -1;
    buf[n] = '/';
    buf[n + 1] = '\0';
    return 0;
}

static int read_target(struct vnode *link, char *buf)
{
    if (!link->mnt->type->readlink)
        return -EIO;
    int64_t n = link->mnt->type->readlink(link, buf, VFS_PATH_MAX);
    if (n < 0)
        return (int)n;
    if (n >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    buf[n] = '\0';
    if (!buf[0])
        return -ENOENT;
    return 0;
}

static int follow_link(struct walk *w, struct vnode *dir)
{
    if (++w->links > VFS_FOLLOW_MAX)
        return -ELOOP;
    if (!w->buf) {
        w->buf = kmalloc(VFS_PATH_MAX);
        w->tgt = kmalloc(VFS_PATH_MAX);
        if (!w->buf || !w->tgt)
            return -ENOMEM;
    }
    int r = read_target(w->cur, w->tgt);
    if (r < 0)
        return r;
    size_t n = strlen(w->tgt), rest = strlen(w->p);
    if (n + rest >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    memmove(w->buf + n, w->p, rest + 1);
    memcpy(w->buf, w->tgt, n);
    w->p = w->buf;
    w->followed = true;
    if (w->buf[0] == '/') {
        struct vnode *root = vfs_root();
        if (!root)
            return -ENOENT;
        vnode_put(w->cur);
        vnode_put(dir);
        w->cur = root;
    } else {
        vnode_put(w->cur);
        w->cur = dir;
    }
    return 0;
}

static int walk_core(struct vnode *start, const char *path, unsigned flags, struct nameidata *nd,
                     struct vnode **out, bool *followed)
{
    struct walk w = { .cur = start_of(start, path), .p = path };
    if (!w.cur)
        return -ENOENT;
    char name[VFS_NAME_MAX];
    bool slash = false;
    int r = 0;
    if (nd) {
        nd->last[0] = '\0';
        nd->slash = false;
    }
    for (;;) {
        int n = next_component(&w.p, name);
        if (n <= 0) {
            r = n < 0 ? n : 0;
            break;
        }
        bool last = only_slashes(w.p);
        slash = *w.p == '/';
        if (nd && last) {
            strlcpy(nd->last, name, sizeof(nd->last));
            nd->slash = slash;
            break;
        }
        struct vnode *dir = w.cur;
        vnode_get(dir);
        r = step(&w.cur, name);
        if (r == 0 && vnode_is_link(w.cur) && (!last || slash || !(flags & WALK_NOFOLLOW))) {
            r = follow_link(&w, dir);
            if (r == 0)
                continue;
        }
        vnode_put(dir);
        if (r < 0)
            break;
    }
    if (r == 0 && nd) {
        if (!vnode_is_dir(w.cur)) {
            r = -ENOTDIR;
        } else {
            nd->dir = w.cur;
            w.cur = NULL;
        }
    } else if (r == 0) {
        if (slash && !vnode_is_dir(w.cur)) {
            r = -ENOTDIR;
        } else {
            *out = w.cur;
            w.cur = NULL;
        }
    }
    vnode_put(w.cur);
    kfree(w.buf);
    kfree(w.tgt);
    if (followed)
        *followed = w.followed;
    return r;
}

int vfs_walk(struct vnode *start, const char *path, struct vnode **out)
{
    return walk_core(start, path, 0, NULL, out, NULL);
}

int vfs_walk_ex(struct vnode *start, const char *path, bool follow, struct vnode **out, bool *followed)
{
    return walk_core(start, path, follow ? 0 : WALK_NOFOLLOW, NULL, out, followed);
}

static int walk_parent(struct vnode *start, const char *path, struct nameidata *nd)
{
    nd->followed = false;
    return walk_core(start, path, 0, nd, NULL, &nd->followed);
}

static int open_create(struct vnode *start, const char *path, int flags, uint32_t mode, struct vnode **out,
                       bool *created, bool *followed)
{
    char *buf = NULL;
    struct vnode *hold = NULL;
    int r = 0;
    for (int depth = 0;; depth++) {
        struct nameidata nd;
        r = walk_parent(start, path, &nd);
        if (r < 0)
            break;
        if (nd.followed)
            *followed = true;
        if (!nd.last[0] || is_dot(nd.last)) {
            vnode_put(nd.dir);
            struct vnode *v = NULL;
            r = walk_core(start, path, 0, NULL, &v, followed);
            vnode_put(v);
            if (r == 0)
                r = -EISDIR;
            break;
        }
        struct mount *m = nd.dir->mnt;
        struct vnode *link = NULL, *v = NULL;
        mutex_lock(&m->ns_lock);
        struct vnode *found = NULL;
        r = nd.dir->unlinked ? -ENOENT : m->type->lookup(nd.dir, nd.last, &found);
        if (r == 0 && vnode_is_link(found)) {
            if (flags & O_EXCL)
                r = -EEXIST;
            else if (flags & O_NOFOLLOW)
                r = -ELOOP;
            else
                link = found;
            if (r < 0)
                vnode_put(found);
        } else if (r == 0) {
            v = follow_mounts(found);
            if (flags & O_EXCL)
                r = -EEXIST;
            else if (nd.slash && !vnode_is_dir(v))
                r = -ENOTDIR;
        } else if (r == -ENOENT && !nd.dir->unlinked) {
            if (nd.slash)
                r = -EISDIR;
            else if (m->flags & MNT_RDONLY)
                r = -EROFS;
            else if (!m->type->create)
                r = -EPERM;
            else {
                r = m->type->create(nd.dir, nd.last, S_IFREG | (mode & 07777), &v);
                *created = r == 0;
            }
        }
        mutex_unlock(&m->ns_lock);
        if (!link) {
            vnode_put(nd.dir);
            if (r < 0)
                vnode_put(v);
            else
                *out = v;
            break;
        }
        if (!buf)
            buf = kmalloc(VFS_PATH_MAX + 1);
        r = buf ? read_target(link, buf) : -ENOMEM;
        if (r == 0 && depth >= VFS_FOLLOW_MAX)
            r = -ELOOP;
        if (r == 0 && nd.slash && strlcat_slash(buf, VFS_PATH_MAX + 1) < 0)
            r = -ENAMETOOLONG;
        vnode_put(link);
        vnode_put(hold);
        hold = nd.dir;
        if (r < 0)
            break;
        *followed = true;
        start = buf[0] == '/' ? NULL : hold;
        path = buf;
    }
    vnode_put(hold);
    kfree(buf);
    return r;
}

static int open_path(struct vnode *start, const char *path, int flags, uint32_t mode, struct file **out,
                     bool *followed)
{
    struct vnode *v = NULL;
    bool created = false;
    int acc = flags & O_ACCMODE;
    bool writing = acc == O_WRONLY || acc == O_RDWR;
    int r;

    if (flags & O_CREAT) {
        r = open_create(start, path, flags, mode, &v, &created, followed);
        if (r < 0)
            return r;
    } else {
        r = walk_core(start, path, (flags & O_NOFOLLOW) ? WALK_NOFOLLOW : 0, NULL, &v, followed);
        if (r < 0)
            return r;
        if (vnode_is_link(v)) {
            vnode_put(v);
            return -ELOOP;
        }
    }

    if ((v->mode & S_IFMT) == S_IFSOCK) {
        r = -ENXIO;
    } else if (vnode_is_dir(v)) {
        if (writing || (flags & (O_CREAT | O_TRUNC)))
            r = -EISDIR;
    } else if (flags & O_DIRECTORY) {
        r = -ENOTDIR;
    } else if (vnode_is_reg(v) && writing && vfs_is_rdonly(v)) {
        r = -EROFS;
    }
    if (r == 0 && (flags & O_TRUNC) && writing && vnode_is_reg(v) && !created && v->size)
        r = vfs_truncate(v, 0);
    if (r < 0) {
        vnode_put(v);
        return r;
    }

    struct file *f = NULL;
    if (v->mnt->type->open && !vnode_is_dir(v)) {
        r = v->mnt->type->open(v, flags, &f);
        if (r == 0) {
            f->vn = v;
            if (!f->mode)
                f->mode = v->mode;
        }
    } else {
        f = file_from_vnode(v, flags);
        r = f ? 0 : -ENOMEM;
    }
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    *out = f;
    return 0;
}

int vfs_open_ex(struct vnode *start, const char *path, int flags, uint32_t mode, struct file **out,
                bool *followed)
{
    bool f = false;
    int r = open_path(start, path, flags, mode, out, &f);
    if (followed)
        *followed = f;
    return r;
}

int vfs_open(struct vnode *start, const char *path, int flags, struct file **out)
{
    return vfs_open_ex(start, path, flags, 0644, out, NULL);
}

int vfs_mknod(struct vnode *start, const char *path, uint32_t mode, struct vnode **out)
{
    struct nameidata nd;
    int r = walk_parent(start, path, &nd);
    if (r < 0)
        return r;
    if (!nd.last[0] || is_dot(nd.last) || nd.slash) {
        vnode_put(nd.dir);
        return -EEXIST;
    }
    struct mount *m = nd.dir->mnt;
    mutex_lock(&m->ns_lock);
    struct vnode *v = NULL;
    r = nd.dir->unlinked ? -ENOENT : m->type->lookup(nd.dir, nd.last, &v);
    if (r == 0) {
        vnode_put(v);
        v = NULL;
        r = -EEXIST;
    } else if (r == -ENOENT && !nd.dir->unlinked) {
        if (m->flags & MNT_RDONLY)
            r = -EROFS;
        else if (!m->type->create)
            r = -EPERM;
        else
            r = m->type->create(nd.dir, nd.last, mode, &v);
    }
    mutex_unlock(&m->ns_lock);
    vnode_put(nd.dir);
    if (r == 0 && (v->mode & S_IFMT) != (mode & S_IFMT)) {
        vnode_put(v);
        return -EPERM;
    }
    if (r == 0)
        *out = v;
    return r;
}

int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)
{
    struct nameidata nd;
    int r = walk_parent(start, path, &nd);
    if (r < 0)
        return r;
    if (!nd.last[0] || is_dot(nd.last)) {
        vnode_put(nd.dir);
        return -EEXIST;
    }
    struct mount *m = nd.dir->mnt;
    mutex_lock(&m->ns_lock);
    struct vnode *v = NULL;
    r = nd.dir->unlinked ? -ENOENT : m->type->lookup(nd.dir, nd.last, &v);
    if (r == 0) {
        vnode_put(v);
        r = -EEXIST;
    } else if (r == -ENOENT && !nd.dir->unlinked) {
        if (m->flags & MNT_RDONLY)
            r = -EROFS;
        else if (!m->type->create)
            r = -EPERM;
        else {
            v = NULL;
            r = m->type->create(nd.dir, nd.last, S_IFDIR | (mode & 07777), &v);
            if (r == 0)
                vnode_put(v);
        }
    }
    mutex_unlock(&m->ns_lock);
    vnode_put(nd.dir);
    return r;
}

int vfs_symlink(struct vnode *start, const char *path, const char *target)
{
    size_t tlen = strlen(target);
    if (!tlen)
        return -ENOENT;
    if (tlen >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    struct nameidata nd;
    int r = walk_parent(start, path, &nd);
    if (r < 0)
        return r;
    if (!nd.last[0] || is_dot(nd.last)) {
        vnode_put(nd.dir);
        return -EEXIST;
    }
    struct mount *m = nd.dir->mnt;
    mutex_lock(&m->ns_lock);
    struct vnode *v = NULL;
    r = nd.dir->unlinked ? -ENOENT : m->type->lookup(nd.dir, nd.last, &v);
    if (r == 0) {
        vnode_put(v);
        r = -EEXIST;
    } else if (r == -ENOENT && !nd.dir->unlinked) {
        if (nd.slash)
            r = -ENOENT;
        else if (m->flags & MNT_RDONLY)
            r = -EROFS;
        else if (!m->type->symlink)
            r = -EPERM;
        else {
            v = NULL;
            r = m->type->symlink(nd.dir, nd.last, target, &v);
            if (r == 0)
                vnode_put(v);
        }
    }
    mutex_unlock(&m->ns_lock);
    vnode_put(nd.dir);
    return r;
}

int vfs_unlink(struct vnode *start, const char *path, bool is_dir)
{
    struct nameidata nd;
    int r = walk_parent(start, path, &nd);
    if (r < 0)
        return r;
    if (!nd.last[0]) {
        vnode_put(nd.dir);
        return is_dir ? -EBUSY : -EISDIR;
    }
    if (is_dot(nd.last)) {
        vnode_put(nd.dir);
        if (!is_dir)
            return -EISDIR;
        return nd.last[1] ? -ENOTEMPTY : -EINVAL;
    }
    struct mount *m = nd.dir->mnt;
    mutex_lock(&m->ns_lock);
    struct vnode *v = NULL;
    r = nd.dir->unlinked ? -ENOENT : m->type->lookup(nd.dir, nd.last, &v);
    if (r == 0) {
        if (v->covered_by)
            r = -EBUSY;
        else if (is_dir && !vnode_is_dir(v))
            r = -ENOTDIR;
        else if (!is_dir && vnode_is_dir(v))
            r = -EISDIR;
        else if (nd.slash && !vnode_is_dir(v))
            r = -ENOTDIR;
        else if (m->flags & MNT_RDONLY)
            r = -EROFS;
        else if (!m->type->unlink)
            r = -EPERM;
        else
            r = m->type->unlink(nd.dir, nd.last, v);
    }
    mutex_unlock(&m->ns_lock);
    vnode_put(v);
    vnode_put(nd.dir);
    return r;
}

static int check_not_ancestor(struct vnode *src, struct vnode *dir)
{
    struct vnode *v = dir;
    vnode_get(v);
    int r = -ELOOP;
    for (int guard = 0; guard < 4096; guard++) {
        if (v == src) {
            r = -EINVAL;
            break;
        }
        if (v == v->mnt->root) {
            r = 0;
            break;
        }
        if (v->unlinked) {
            r = -ENOENT;
            break;
        }
        struct vnode *up = NULL;
        int e = v->mnt->type->lookup(v, "..", &up);
        if (e < 0) {
            r = e;
            break;
        }
        vnode_put(v);
        v = up;
    }
    vnode_put(v);
    return r;
}

static int rename_locked(struct mount *m, struct nameidata *a, struct nameidata *b)
{
    if (a->dir->unlinked || b->dir->unlinked)
        return -ENOENT;
    struct vnode *sv = NULL, *dv = NULL;
    int r = m->type->lookup(a->dir, a->last, &sv);
    if (r < 0)
        return r;
    if (sv->covered_by)
        r = -EBUSY;
    else if ((a->slash || b->slash) && !vnode_is_dir(sv))
        r = -ENOTDIR;
    if (r == 0) {
        int e = m->type->lookup(b->dir, b->last, &dv);
        if (e < 0 && e != -ENOENT)
            r = e;
        if (e < 0)
            dv = NULL;
    }
    if (r == 0 && dv == sv) {
        vnode_put(dv);
        dv = NULL;
        if (a->dir != b->dir || strcmp(a->last, b->last) == 0) {
            vnode_put(sv);
            return 0;
        }
    }
    if (r == 0 && dv) {
        if (dv->covered_by)
            r = -EBUSY;
        else if (vnode_is_dir(sv) && !vnode_is_dir(dv))
            r = -ENOTDIR;
        else if (!vnode_is_dir(sv) && vnode_is_dir(dv))
            r = -EISDIR;
    }
    if (r == 0 && vnode_is_dir(sv))
        r = check_not_ancestor(sv, b->dir);
    if (r == 0)
        r = m->type->rename(a->dir, a->last, b->dir, b->last, sv, dv);
    vnode_put(dv);
    vnode_put(sv);
    return r;
}

int vfs_rename(struct vnode *ostart, const char *opath, struct vnode *nstart, const char *npath)
{
    struct nameidata a, b;
    int r = walk_parent(ostart, opath, &a);
    if (r < 0)
        return r;
    r = walk_parent(nstart, npath, &b);
    if (r < 0) {
        vnode_put(a.dir);
        return r;
    }
    struct mount *m = a.dir->mnt;
    if (!a.last[0] || !b.last[0] || is_dot(a.last) || is_dot(b.last))
        r = -EBUSY;
    else if (b.dir->mnt != m)
        r = -EXDEV;
    else if (m->flags & MNT_RDONLY)
        r = -EROFS;
    else if (!m->type->rename)
        r = -EPERM;
    if (r == 0) {
        mutex_lock(&m->ns_lock);
        r = rename_locked(m, &a, &b);
        mutex_unlock(&m->ns_lock);
    }
    vnode_put(a.dir);
    vnode_put(b.dir);
    return r;
}

int64_t vfs_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len)
{
    if (vnode_is_dir(v))
        return -EISDIR;
    if (pcache_enabled(v))
        return pcache_read(v, offset, buf, len);
    if (!v->mnt->type->read)
        return -EINVAL;
    return v->mnt->type->read(v, offset, buf, len);
}

int64_t vfs_write(struct vnode *v, uint64_t *offset, const void *buf, uint64_t len, bool append)
{
    if (vnode_is_dir(v))
        return -EISDIR;
    if (vfs_is_rdonly(v))
        return -EROFS;
    if (!v->mnt->type->write)
        return -EINVAL;
    return v->mnt->type->write(v, offset, buf, len, append);
}

int vfs_truncate(struct vnode *v, uint64_t size)
{
    if (vnode_is_dir(v))
        return -EISDIR;
    if (!vnode_is_reg(v))
        return -EINVAL;
    if (vfs_is_rdonly(v))
        return -EROFS;
    if (!v->mnt->type->truncate)
        return -EPERM;
    return v->mnt->type->truncate(v, size);
}

int vfs_readdir_vnode(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx)
{
    if (!vnode_is_dir(dir))
        return -ENOTDIR;
    if (dir->unlinked || !dir->mnt->type->readdir)
        return 0;
    return dir->mnt->type->readdir(dir, pos, cb, ctx);
}

bool vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx)
{
    struct vnode *v;
    if (vfs_walk(NULL, path, &v) < 0)
        return false;
    uint64_t pos = 0;
    int r = vfs_readdir_vnode(v, &pos, cb, ctx);
    vnode_put(v);
    return r >= 0;
}

struct name_ctx {
    uint64_t ino;
    char *name;
    bool found;
};

static bool name_cb(const struct vfs_dirent *e, void *ctx)
{
    struct name_ctx *n = ctx;
    if (e->ino != n->ino)
        return true;
    strlcpy(n->name, e->name, VFS_NAME_MAX);
    n->found = true;
    return false;
}

int vfs_getpath(struct vnode *v, char *buf, size_t size)
{
    if (size < 2)
        return -ERANGE;
    char *name = kmalloc(VFS_NAME_MAX);
    if (!name)
        return -ENOMEM;
    size_t at = size - 1;
    buf[at] = '\0';
    vnode_get(v);
    int r = -ENAMETOOLONG;
    for (int guard = 0; guard < 1024; guard++) {
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        while (v != root_mount->root && v == v->mnt->root && v->mnt->covered) {
            struct vnode *c = v->mnt->covered;
            c->refs++;
            v->refs--;
            v = c;
        }
        bool at_root = v == root_mount->root;
        spin_unlock_irqrestore(&vfs_lock, fl);
        if (at_root) {
            r = 0;
            break;
        }
        if (v->unlinked || v == v->mnt->root) {
            r = -ENOENT;
            break;
        }
        struct vnode *parent = NULL;
        r = v->mnt->type->lookup(v, "..", &parent);
        if (r < 0)
            break;
        if (parent == v) {
            vnode_put(parent);
            r = -ENOENT;
            break;
        }
        struct name_ctx nc = { .ino = v->ino, .name = name, .found = false };
        uint64_t pos = 0;
        vfs_readdir_vnode(parent, &pos, name_cb, &nc);
        vnode_put(v);
        v = parent;
        if (!nc.found) {
            r = -ENOENT;
            break;
        }
        size_t n = strlen(name);
        if (n + 1 > at) {
            r = -ENAMETOOLONG;
            break;
        }
        at -= n;
        memcpy(buf + at, name, n);
        buf[--at] = '/';
        r = -ENAMETOOLONG;
    }
    vnode_put(v);
    kfree(name);
    if (r < 0)
        return r;
    if (at == size - 1)
        buf[--at] = '/';
    memmove(buf, buf + at, size - at);
    return 0;
}

void *vfs_read_vnode(struct vnode *v, uint64_t *size_out)
{
    if (!vnode_is_reg(v) || v->size > VFS_READ_ALL_MAX)
        return NULL;
    uint64_t size = v->size;
    void *buf = kmalloc(size ? size : 1);
    if (!buf)
        return NULL;
    uint64_t done = 0;
    while (done < size) {
        int64_t n = vfs_read(v, done, (uint8_t *)buf + done, size - done);
        if (n <= 0) {
            kfree(buf);
            return NULL;
        }
        done += (uint64_t)n;
    }
    if (size_out)
        *size_out = size;
    return buf;
}

void *vfs_read_all(const char *path, uint64_t *size_out)
{
    struct vnode *v;
    if (vfs_walk(NULL, path, &v) < 0)
        return NULL;
    void *buf = vfs_read_vnode(v, size_out);
    vnode_put(v);
    return buf;
}

static bool dev_busy_locked(struct blockdev *dev)
{
    for (struct mount *m = mounts; m; m = m->next) {
        if (!m->dev)
            continue;
        if (m->dev == dev || m->dev->parent == dev || dev->parent == m->dev)
            return true;
    }
    return false;
}

static void mount_free(struct mount *m)
{
    vnode_put(m->root);
    if (m->type->unmount)
        m->type->unmount(m);
    mutex_deinit(&m->ns_lock);
    kfree(m);
}

int vfs_mount(const struct fs_type *type, struct blockdev *dev, const char *source,
              struct vnode *target, const char *point, uint32_t flags, const char *data)
{
    if (type->needs_dev && !dev)
        return -ENOTBLK;
    mutex_lock(&mount_lock);
    int r = 0;
    if (target && !vnode_is_dir(target))
        r = -ENOTDIR;
    else if (target && target == target->mnt->root)
        r = -EBUSY;
    else if (!target && root_mount)
        r = -EBUSY;
    else if (dev && dev_busy_locked(dev))
        r = -EBUSY;
    struct mount *m = NULL;
    if (r == 0) {
        m = kzalloc(sizeof(*m));
        if (!m)
            r = -ENOMEM;
    }
    if (r == 0) {
        m->type = type;
        m->subtype = type->name;
        m->dev = type->needs_dev ? dev : NULL;
        m->flags = flags & MNT_RDONLY;
        m->id = next_mount_id++;
        strlcpy(m->source, source ? source : type->name, sizeof(m->source));
        strlcpy(m->point, point, sizeof(m->point));
        mutex_init(&m->ns_lock, "vfs-ns");
        r = type->mount(m, m->dev, data);
        if (r < 0) {
            mutex_deinit(&m->ns_lock);
            kfree(m);
            m = NULL;
        }
    }
    if (r == 0 && target) {
        mutex_lock(&target->mnt->ns_lock);
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        if (target->unlinked)
            r = -ENOENT;
        else if (target->covered_by)
            r = -EBUSY;
        else {
            target->covered_by = m;
            target->refs++;
            m->covered = target;
        }
        spin_unlock_irqrestore(&vfs_lock, fl);
        mutex_unlock(&target->mnt->ns_lock);
        if (r < 0)
            mount_free(m);
    }
    if (r == 0) {
        struct mount **pp = &mounts;
        while (*pp)
            pp = &(*pp)->next;
        *pp = m;
        if (!target) {
            uint64_t fl = spin_lock_irqsave(&vfs_lock);
            root_mount = m;
            spin_unlock_irqrestore(&vfs_lock, fl);
        }
    }
    mutex_unlock(&mount_lock);
    return r;
}

int vfs_umount(struct vnode *root)
{
    mutex_lock(&mount_lock);
    struct mount *m = root->mnt;
    int r = 0;
    if (root != m->root || !m->covered) {
        r = -EINVAL;
    } else {
        evict_mount(m);
        uint64_t fl = spin_lock_irqsave(&vfs_lock);
        if (root->refs != 2 || m->nvnodes != 1)
            r = -EBUSY;
        else
            m->covered->covered_by = NULL;
        spin_unlock_irqrestore(&vfs_lock, fl);
    }
    if (r < 0) {
        mutex_unlock(&mount_lock);
        vnode_put(root);
        return r;
    }
    struct mount **pp = &mounts;
    while (*pp && *pp != m)
        pp = &(*pp)->next;
    if (*pp)
        *pp = m->next;
    if (m->type->sync)
        m->type->sync(m);
    vnode_put(root);
    struct vnode *covered = m->covered;
    log_info("vfs", "%s desmontado de %s", m->source, m->point);
    mount_free(m);
    mutex_unlock(&mount_lock);
    vnode_put(covered);
    return 0;
}

bool vfs_sync(void)
{
    bool ok = true;
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next)
        if (m->type->sync && m->type->sync(m) < 0)
            ok = false;
    mutex_unlock(&mount_lock);
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

int vfs_mount_kernel(const char *type, struct blockdev *dev, const char *source, const char *point)
{
    const struct fs_type *t = vfs_find_type(type);
    if (!t)
        return -ENODEV;
    struct vnode *target;
    int r = vfs_walk(NULL, point, &target);
    if (r == -ENOENT && vfs_mkdir(NULL, point, 0755) == 0)
        r = vfs_walk(NULL, point, &target);
    if (r < 0)
        return r;
    r = vfs_mount(t, dev, source, target, point, 0, NULL);
    vnode_put(target);
    return r;
}

void vfs_mounts_foreach(void (*cb)(const struct mount *m, void *ctx), void *ctx)
{
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next)
        cb(m, ctx);
    mutex_unlock(&mount_lock);
}

const char *vfs_mnt_device(void)
{
    const char *name = NULL;
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next)
        if (m->dev && m->covered && strcmp(m->point, "/mnt") == 0)
            name = m->dev->name;
    mutex_unlock(&mount_lock);
    return name;
}

bool vfs_mount_extra(void)
{
    if (vfs_mnt_device())
        return true;
    struct blockdev *root_dev = root_mount ? root_mount->dev : NULL;
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_at(i);
        if (dev == root_dev || (root_dev && dev == root_dev->parent))
            continue;
        if (root_dev && dev->parent && dev->parent == root_dev->parent)
            continue;
        char src[VFS_SOURCE_MAX];
        ksnprintf(src, sizeof(src), "/dev/%s", dev->name);
        if (vfs_mount_kernel("vfat", dev, src, "/mnt") == 0) {
            const char *sub = "-";
            mutex_lock(&mount_lock);
            for (struct mount *m = mounts; m; m = m->next)
                if (m->dev == dev)
                    sub = m->subtype;
            mutex_unlock(&mount_lock);
            log_info("vfs", "/mnt montado: %s em %s", sub, dev->name);
            return true;
        }
    }
    return false;
}

void vfs_init(void)
{
    spinlock_init(&vfs_lock);
    mutex_init(&mount_lock, "vfs-mount");
    pcache_init();
    for (uint32_t i = 0; i < blockdev_count() && !root_mount; i++) {
        struct blockdev *dev = blockdev_at(i);
        if (!dev->parent || dev->part_kind != PART_ROOT)
            continue;
        char src[VFS_SOURCE_MAX];
        ksnprintf(src, sizeof(src), "/dev/%s", dev->name);
        if (vfs_mount(&ext2_fs_type, dev, src, NULL, "/", 0, NULL) == 0)
            log_info("vfs", "raiz montada: %s em %s", root_mount->subtype, dev->name);
        else
            log_warn("vfs", "%s e uma particao de raiz, mas nao montou como ext2", dev->name);
    }
    for (uint32_t i = 0; i < blockdev_count() && !root_mount; i++) {
        struct blockdev *dev = blockdev_at(i);
        if (!dev->parent)
            continue;
        char src[VFS_SOURCE_MAX];
        ksnprintf(src, sizeof(src), "/dev/%s", dev->name);
        if (vfs_mount(&fat_fs_type, dev, src, NULL, "/", 0, NULL) == 0)
            log_info("vfs", "raiz montada: %s em %s", root_mount->subtype, dev->name);
    }
    if (!root_mount) {
        log_warn("vfs", "nenhum sistema de arquivos reconhecido");
        return;
    }
    if (root_mount->type != &ext2_fs_type)
        return;
    for (uint32_t i = 0; i < blockdev_count(); i++) {
        struct blockdev *dev = blockdev_at(i);
        if (dev->part_kind != PART_ESP || dev->parent != root_mount->dev->parent)
            continue;
        char src[VFS_SOURCE_MAX];
        ksnprintf(src, sizeof(src), "/dev/%s", dev->name);
        int r = vfs_mount_kernel("vfat", dev, src, "/boot/efi");
        if (r == 0)
            log_info("vfs", "particao EFI %s montada em /boot/efi", dev->name);
        else
            log_warn("vfs", "particao EFI %s nao montou em /boot/efi (erro %d)", dev->name, -r);
        break;
    }
}
