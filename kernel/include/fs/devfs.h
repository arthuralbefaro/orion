#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>

struct file;

#define DEVFS_ROOT "/dev"

void devfs_init(void);
bool devfs_owns(const char *path);
bool devfs_is_root(const char *path);
bool devfs_lookup(const char *path, struct vfs_node *out);
uint32_t devfs_mode(const char *path);
struct file *devfs_open(const char *path);
bool devfs_readdir(vfs_dir_cb cb, void *ctx);
void devfs_dump(void);
