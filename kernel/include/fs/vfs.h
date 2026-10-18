#pragma once

#include <kernel/compiler.h>
#include <kernel/mutex.h>
#include <kernel/syscall.h>
#include <fs/pcache.h>

#define VFS_NAME_MAX   256
#define VFS_PATH_MAX   512
#define VFS_SOURCE_MAX 32
#define VFS_READ_ALL_MAX (64ull * 1024 * 1024)
#define VFS_FOLLOW_MAX 40

#define MNT_RDONLY 1u

#define VA_MODE      1u
#define VA_UID       2u
#define VA_GID       4u
#define VA_ATIME     8u
#define VA_MTIME     16u
#define VA_KILL_SUID 32u

#define VFS_MAY_EXEC  1
#define VFS_MAY_WRITE 2
#define VFS_MAY_READ  4

struct vnode;
struct mount;
struct file;
struct blockdev;

struct vfs_dirent {
    char name[VFS_NAME_MAX];
    uint64_t ino;
    uint64_t size;
    uint64_t next;
    uint32_t mode;
};

struct vfs_time {
    int64_t sec;
    uint32_t nsec;
};

struct vattr {
    uint32_t mask;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t blocks;
    struct vfs_time atime;
    struct vfs_time mtime;
    struct vfs_time ctime;
};

typedef bool (*vfs_dir_cb)(const struct vfs_dirent *entry, void *ctx);

struct fs_type {
    const char *name;
    bool needs_dev;
    int (*mount)(struct mount *m, struct blockdev *dev, const char *data);
    void (*unmount)(struct mount *m);
    int (*sync)(struct mount *m);
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*readdir)(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx);
    int64_t (*read)(struct vnode *v, uint64_t offset, void *buf, uint64_t len);
    int (*readpage)(struct vnode *v, uint64_t idx, uint64_t *page);
    int64_t (*readlink)(struct vnode *v, char *buf, uint64_t size);
    int64_t (*write)(struct vnode *v, uint64_t *offset, const void *buf, uint64_t len, bool append);
    int (*truncate)(struct vnode *v, uint64_t size);
    int (*create)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out);
    int (*symlink)(struct vnode *dir, const char *name, const char *target, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name, struct vnode *victim);
    int (*rename)(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname,
                  struct vnode *v, struct vnode *victim);
    int (*getattr)(struct vnode *v, struct vattr *out);
    int (*setattr)(struct vnode *v, const struct vattr *in);
    int (*open)(struct vnode *v, int flags, struct file **out);
    void (*release)(struct vnode *v);
};

struct vnode {
    struct mount *mnt;
    uint64_t key;
    uint64_t ino;
    uint32_t mode;
    int32_t refs;
    uint64_t size;
    bool hashed;
    bool unlinked;
    bool displaced;
    bool inactive;
    bool evicting;
    uint64_t lru_pages;
    struct vnode *hnext;
    struct vnode *dnext;
    struct vnode *lru_prev;
    struct vnode *lru_next;
    struct pcache pc;
    struct mount *covered_by;
    struct mutex io_lock;
    void *priv;
};

struct mount {
    const struct fs_type *type;
    const char *subtype;
    void *priv;
    struct blockdev *dev;
    struct vnode *root;
    struct vnode *covered;
    struct mount *next;
    uint32_t flags;
    uint32_t id;
    uint32_t nvnodes;
    struct vnode *displaced;
    uint32_t evicting;
    char source[VFS_SOURCE_MAX];
    char point[VFS_PATH_MAX];
    struct mutex ns_lock;
};

static inline bool vnode_is_dir(const struct vnode *v)
{
    return (v->mode & S_IFMT) == S_IFDIR;
}

static inline bool vnode_is_reg(const struct vnode *v)
{
    return (v->mode & S_IFMT) == S_IFREG;
}

static inline bool vnode_is_link(const struct vnode *v)
{
    return (v->mode & S_IFMT) == S_IFLNK;
}

void vfs_init(void);
bool vfs_mounted(void);
const char *vfs_root_type(void);
const struct fs_type *vfs_find_type(const char *name);

struct vnode *vnode_new(struct mount *m, uint64_t key, uint32_t mode);
struct vnode *vcache_find(struct mount *m, uint64_t key);
uint64_t vcache_ino(struct mount *m, uint64_t key);
struct vnode *vcache_insert(struct vnode *v);
void vcache_rekey(struct vnode *v, uint64_t key);
void vcache_unhash(struct vnode *v);
void vnode_get(struct vnode *v);
void vnode_put(struct vnode *v);
void vfs_cache_shrink(void);
uint64_t vfs_cache_reclaim(uint64_t want);
uint64_t vfs_cache_reclaimable(void);
struct vnode *vfs_root(void);

int vfs_walk(struct vnode *start, const char *path, struct vnode **out);
int vfs_walk_ex(struct vnode *start, const char *path, bool follow, struct vnode **out, bool *followed);
int vfs_open(struct vnode *start, const char *path, int flags, struct file **out);
int vfs_open_ex(struct vnode *start, const char *path, int flags, uint32_t mode, struct file **out,
                bool *followed);
int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode);
int vfs_mknod(struct vnode *start, const char *path, uint32_t mode, struct vnode **out);
int vfs_symlink(struct vnode *start, const char *path, const char *target);
int vfs_unlink(struct vnode *start, const char *path, bool is_dir);
int vfs_rename(struct vnode *ostart, const char *opath, struct vnode *nstart, const char *npath);

int64_t vfs_read(struct vnode *v, uint64_t offset, void *buf, uint64_t len);
int64_t vfs_write(struct vnode *v, uint64_t *offset, const void *buf, uint64_t len, bool append);
int vfs_truncate(struct vnode *v, uint64_t size);
void vfs_getattr(struct vnode *v, struct vattr *out);
struct cred;
int vfs_permission(struct vnode *v, int mask);
int vfs_permission_as(struct vnode *v, int mask, const struct cred *c, bool real);
int vfs_setattr(struct vnode *v, const struct vattr *in);
void vfs_now(struct vfs_time *t);
uint32_t vattr_apply_mode(uint32_t old, const struct vattr *in);
int vfs_readdir_vnode(struct vnode *dir, uint64_t *pos, vfs_dir_cb cb, void *ctx);
int vfs_getpath(struct vnode *v, char *buf, size_t size);
bool vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx);
void *vfs_read_vnode(struct vnode *v, uint64_t *size_out);
void *vfs_read_all(const char *path, uint64_t *size_out);
bool vfs_is_rdonly(const struct vnode *v);

int vfs_mount(const struct fs_type *type, struct blockdev *dev, const char *source,
              struct vnode *target, const char *point, uint32_t flags, const char *data);
int vfs_umount(struct vnode *root);
bool vfs_sync(void);
void vfs_flush_init(void);
bool vfs_mount_extra(void);
const char *vfs_mnt_device(void);
void vfs_mounts_foreach(void (*cb)(const struct mount *m, void *ctx), void *ctx);
int vfs_mount_kernel(const char *type, struct blockdev *dev, const char *source, const char *point);
