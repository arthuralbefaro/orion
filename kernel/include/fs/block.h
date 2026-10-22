#pragma once

#include <kernel/compiler.h>

#define BLOCK_NAME_MAX 16
#define BLOCK_MAX_DEVICES 32
#define BLOCK_CACHE_ENTRIES 256
#define BLOCK_SECTOR 512
#define BLOCK_DIRECT_MIN 8

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
    uint8_t part_kind;
    uint8_t type_guid[16];
    uint64_t rd_ios;
    uint64_t rd_sectors;
    uint64_t wr_ios;
    uint64_t wr_sectors;
    uint32_t refs;
    bool gone;
    bool released;
    bool removable;
    char model[41];
    void (*release)(struct blockdev *dev);
    bool (*flush)(struct blockdev *dev);
};

#define PART_OTHER 0
#define PART_ESP   1
#define PART_ROOT  2
#define PART_DATA  3
#define PART_GPT   0xEE

struct blockdev *blockdev_register(const char *name, uint32_t sector_size, uint64_t sector_count,
                                   blockdev_read_fn read, blockdev_write_fn write, void *priv);
bool blockdev_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
bool blockdev_read_cached(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
bool blockdev_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf);
bool blockdev_sync(void);
bool blockdev_writable(const struct blockdev *dev);
void blockdev_cache_init(void);

void blockdev_get(struct blockdev *dev);
void blockdev_put(struct blockdev *dev);
void blockdev_remove(struct blockdev *disk);
bool blockdev_is_gone(struct blockdev *dev);

struct blockdev *blockdev_find(const char *name);
struct blockdev *blockdev_at(uint32_t index);
uint32_t blockdev_count(void);
void blockdev_scan_partitions(struct blockdev *disk);
int blockdev_rescan(struct blockdev *disk);
void blockdev_dump(void);
void blockdev_cache_stats(uint64_t *hits, uint64_t *misses, uint64_t *writes, uint32_t *dirty);
