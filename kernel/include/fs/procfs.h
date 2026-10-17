#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>

extern const struct fs_type procfs_type;

void procfs_init(void);
