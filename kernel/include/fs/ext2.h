#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>

struct ext2_rs;

struct ext2_info {
    uint32_t block_size;
    uint32_t blocks;
    uint32_t free_blocks;
    uint32_t inodes;
    uint32_t free_inodes;
    uint32_t groups;
    uint32_t rev;
    uint32_t inode_size;
    uint32_t writable;
    char label[17];
};

struct ext2_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t blocks;
};

struct ext2_attr {
    uint32_t mask;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t atime;
    uint32_t mtime;
};

#define EXT2_ATTR_MODE  1u
#define EXT2_ATTR_UID   2u
#define EXT2_ATTR_GID   4u
#define EXT2_ATTR_ATIME 8u
#define EXT2_ATTR_MTIME 16u

typedef int (*ext2_read_fn)(void *ctx, uint64_t off, uint8_t *buf, uint32_t len, bool meta);
typedef int (*ext2_write_fn)(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len);
typedef bool (*ext2_dir_fn)(void *ctx, const uint8_t *name, uint32_t len, uint32_t ino, uint8_t dtype,
                            uint64_t next);

int ext2_mount(void *ctx, ext2_read_fn read, ext2_write_fn write, struct ext2_rs **out, struct ext2_info *info);
void ext2_umount(struct ext2_rs *fs);
uint32_t ext2_root_ino(void);
int ext2_stat(const struct ext2_rs *fs, uint32_t ino, struct ext2_stat *out);
int ext2_lookup(const struct ext2_rs *fs, uint32_t dir, const uint8_t *name, uint32_t len, uint32_t *out);
int ext2_readdir(const struct ext2_rs *fs, uint32_t dir, uint64_t *pos, ext2_dir_fn cb, void *ctx);
int64_t ext2_read(const struct ext2_rs *fs, uint32_t ino, uint64_t off, uint8_t *buf, uint64_t len);
int64_t ext2_readlink(const struct ext2_rs *fs, uint32_t ino, uint8_t *buf, uint64_t size);
int64_t ext2_write(struct ext2_rs *fs, uint32_t ino, uint64_t off, const uint8_t *buf, uint64_t len, uint32_t now);
int ext2_truncate(struct ext2_rs *fs, uint32_t ino, uint64_t size, uint32_t now);
int ext2_sync(struct ext2_rs *fs, uint32_t now, bool clean);
int ext2_create(struct ext2_rs *fs, uint32_t dir, const uint8_t *name, uint32_t len, uint32_t mode, uint32_t now,
                uint32_t *out);
int ext2_unlink(struct ext2_rs *fs, uint32_t dir, const uint8_t *name, uint32_t len, uint32_t expect, bool rmdir,
                uint32_t now, uint32_t *nlink);
int ext2_rename(struct ext2_rs *fs, uint32_t odir, const uint8_t *oname, uint32_t olen, uint32_t ndir,
                const uint8_t *nname, uint32_t nlen, uint32_t expect, uint32_t now, uint32_t *victim, uint32_t *nlink);
int ext2_evict(struct ext2_rs *fs, uint32_t ino, uint32_t now);
int ext2_symlink(struct ext2_rs *fs, uint32_t dir, const uint8_t *name, uint32_t len, const uint8_t *target,
                 uint32_t tlen, uint32_t now, uint32_t *out);
int ext2_setattr(struct ext2_rs *fs, uint32_t ino, const struct ext2_attr *attr, uint32_t now);

extern const struct fs_type ext2_fs_type;
