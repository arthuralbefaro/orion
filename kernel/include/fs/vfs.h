#pragma once

#include <kernel/compiler.h>

#define VFS_NAME_MAX 256
#define VFS_PATH_MAX 512

struct filesystem;

struct vfs_node {
    char name[VFS_NAME_MAX];
    bool is_dir;
    uint64_t size;
    uint64_t ino;
    uint64_t dir_ino;
    uint32_t dir_offset;
    struct filesystem *fs;
};

typedef bool (*vfs_dir_cb)(const struct vfs_node *entry, void *ctx);

struct fs_ops {
    bool (*root)(struct filesystem *fs, struct vfs_node *out);
    bool (*lookup)(struct filesystem *fs, const struct vfs_node *dir, const char *name, struct vfs_node *out);
    int64_t (*read)(struct filesystem *fs, const struct vfs_node *node, uint64_t offset, void *buf, uint64_t len);
    bool (*readdir)(struct filesystem *fs, const struct vfs_node *dir, vfs_dir_cb cb, void *ctx);
    int64_t (*write)(struct filesystem *fs, struct vfs_node *node, uint64_t offset, const void *buf, uint64_t len);
    bool (*create)(struct filesystem *fs, const struct vfs_node *dir, const char *name, bool is_dir,
                   struct vfs_node *out);
    bool (*unlink)(struct filesystem *fs, const struct vfs_node *dir, const char *name, bool is_dir);
    bool (*truncate)(struct filesystem *fs, struct vfs_node *node, uint64_t size);
    bool (*rename)(struct filesystem *fs, const struct vfs_node *dir, const char *from, const char *to);
    bool (*sync)(struct filesystem *fs);
};

struct filesystem {
    const char *type;
    const struct fs_ops *ops;
    void *priv;
};

void vfs_init(void);
bool vfs_mounted(void);
const char *vfs_root_type(void);

bool vfs_lookup(const char *path, struct vfs_node *out);
int64_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buf, uint64_t len);
bool vfs_readdir(const char *path, vfs_dir_cb cb, void *ctx);
void *vfs_read_all(const char *path, uint64_t *size_out);

int64_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buf, uint64_t len);
bool vfs_create(const char *path, bool is_dir, struct vfs_node *out);
bool vfs_unlink(const char *path, bool is_dir);
bool vfs_truncate(struct vfs_node *node, uint64_t size);
bool vfs_rename(const char *from, const char *to);
bool vfs_sync(void);
void vfs_flush_init(void);
bool vfs_mount_extra(void);
const char *vfs_mnt_device(void);
bool vfs_writable(void);
