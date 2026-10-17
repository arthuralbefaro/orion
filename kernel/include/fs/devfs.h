#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>

#define DEVFS_ROOT "/dev"

extern const struct fs_type devfs_type;

void devfs_init(void);
struct blockdev *devfs_vnode_blockdev(const struct vnode *v);
void devfs_dump(void);
