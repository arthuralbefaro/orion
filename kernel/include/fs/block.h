#pragma once

#include <kernel/compiler.h>

#define BLOCK_NAME_MAX 16
#define BLOCK_MAX_DEVICES 16
#define BLOCK_CACHE_ENTRIES 256
#define BLOCK_SECTOR 512

struct blockdev;

typedef bool (*blockdev_read_fn)(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
typedef bool (*blockdev_write_fn)(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf);

struct blockdev {
    char name[BLOCK_NAME_MAX];
    uint32_t sector_size;
    uint64_t sector_count;
    uint64_t lba_offset;
    struct blockdev *parent;
    blockdev_read_fn read;
    blockdev_write_fn write;
    void *priv;
    uint8_t part_type;
    uint64_t rd_ios;
    uint64_t rd_sectors;
    uint64_t wr_ios;
    uint64_t wr_sectors;
};

struct blockdev *blockdev_register(const char *name, uint32_t sector_size, uint64_t sector_count,
                                   blockdev_read_fn read, blockdev_write_fn write, void *priv);
bool blockdev_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
bool blockdev_read_cached(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
bool blockdev_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf);
bool blockdev_sync(void);
void blockdev_cache_init(void);

struct blockdev *blockdev_find(const char *name);
struct blockdev *blockdev_at(uint32_t index);
uint32_t blockdev_count(void);
void blockdev_scan_partitions(struct blockdev *disk);
void blockdev_dump(void);
void blockdev_cache_stats(uint64_t *hits, uint64_t *misses, uint64_t *writes, uint32_t *dirty);
