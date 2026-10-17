#pragma once

#include <kernel/compiler.h>
#include <fs/vfs.h>

#define TMPFS_RADIX_LEVELS 3
#define TMPFS_RESERVE_MIN  2048
#define TMPFS_MAX_FILE (1ull << (12 + 9 * TMPFS_RADIX_LEVELS))

extern const struct fs_type tmpfs_type;

uint64_t tmpfs_used_pages(struct mount *m);
void tmpfs_init(void);
