#pragma once

#include <kernel/compiler.h>

#define BLOCK_NAME_MAX 16
#define BLOCK_MAX_DEVICES 16

struct blockdev;

typedef bool (*blockdev_read_fn)(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);

struct blockdev {
    char name[BLOCK_NAME_MAX];
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t lba_offset;
    struct blockdev *parent;
    blockdev_read_fn read;
    void *priv;
    uint8_t part_type;
};

struct blockdev *blockdev_register(const char *name, uint32_t sector_size, uint64_t sector_count,
                                   blockdev_read_fn read, void *priv);
bool blockdev_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
struct blockdev *blockdev_find(const char *name);
struct blockdev *blockdev_at(uint32_t index);
uint32_t blockdev_count(void);
void blockdev_scan_partitions(struct blockdev *disk);
void blockdev_dump(void);
