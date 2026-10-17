#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>
#include <kernel/wait.h>

struct file;

struct file_ops {
    int64_t (*read)(struct file *f, void *buf, uint64_t len);
    int64_t (*write)(struct file *f, const void *buf, uint64_t len);
    void    (*close)(struct file *f);
    int     (*readdir)(struct file *f, uint64_t index, struct vfs_node *out);
    long    (*ioctl)(struct file *f, uint64_t request, uint64_t arg);
    int64_t (*mmap)(struct file *f, uint64_t len, int prot, uint64_t offset);
    short   (*poll)(struct file *f, struct poll_table *pt);
};

struct file {
    const struct file_ops *ops;
    void *priv;
    uint64_t offset;
    int32_t refcount;
    uint32_t mode;
    uint32_t flags;
    struct vfs_node node;
    bool has_node;
    char *path;
};

struct file *file_alloc(const struct file_ops *ops, void *priv);
void file_ref(struct file *f);
void file_unref(struct file *f);

bool file_is_tty(const struct file *f);
bool file_is_regular(const struct file *f);
struct file *file_open_tty(void);
struct file *file_open_path(const char *path, int flags);
bool file_make_pipe(struct file **reader, struct file **writer);

int64_t file_read(struct file *f, void *buf, uint64_t len);
int64_t file_write(struct file *f, const void *buf, uint64_t len);
int     file_readdir(struct file *f, uint64_t index, struct vfs_node *out);
long    file_ioctl(struct file *f, uint64_t request, uint64_t arg);
int64_t file_mmap(struct file *f, uint64_t len, int prot, uint64_t offset);
short   file_poll(struct file *f, struct poll_table *pt);
