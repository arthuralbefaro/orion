#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/mutex.h>
#include <mm/heap.h>
#include <lib/string.h>

static struct blockdev devices[BLOCK_MAX_DEVICES];
static uint32_t device_count;

struct cache_entry {
    struct blockdev *dev;
    uint64_t lba;
    uint64_t stamp;
    bool valid;
    bool dirty;
    uint8_t data[BLOCK_SECTOR];
};

static struct cache_entry *cache;
static struct mutex cache_lock = MUTEX_INIT("bcache");
static uint64_t cache_clock;
static uint64_t stat_hits, stat_misses, stat_writes;

struct blockdev *blockdev_register(const char *name, uint32_t sector_size, uint64_t sector_count,
                                   blockdev_read_fn read, blockdev_write_fn write, void *priv)
{
    if (device_count >= BLOCK_MAX_DEVICES)
        return NULL;
    struct blockdev *d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    d->sector_size = sector_size;
    d->sector_count = sector_count;
    d->read = read;
    d->write = write;
    d->priv = priv;
    return d;
}

static struct blockdev *resolve(struct blockdev *dev, uint64_t *lba)
{
    while (dev->parent) {
        *lba += dev->lba_offset;
        dev = dev->parent;
    }
    return dev;
}

static bool raw_read(struct blockdev *base, uint64_t lba, uint32_t count, void *buf)
{
    __atomic_add_fetch(&base->rd_ios, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&base->rd_sectors, count, __ATOMIC_RELAXED);
    return base->read && base->read(base, lba, count, buf);
}

static bool raw_write(struct blockdev *base, uint64_t lba, uint32_t count, const void *buf)
{
    __atomic_add_fetch(&base->wr_ios, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&base->wr_sectors, count, __ATOMIC_RELAXED);
    return base->write && base->write(base, lba, count, buf);
}

static struct cache_entry *cache_lookup(struct blockdev *base, uint64_t lba)
{
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; i++)
        if (cache[i].valid && cache[i].dev == base && cache[i].lba == lba)
            return &cache[i];
    return NULL;
}

static struct cache_entry *cache_victim(void)
{
    struct cache_entry *best = &cache[0];
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (!cache[i].valid)
            return &cache[i];
        if (cache[i].stamp < best->stamp)
            best = &cache[i];
    }
    if (best->dirty) {
        if (raw_write(best->dev, best->lba, 1, best->data))
            stat_writes++;
        best->dirty = false;
    }
    return best;
}

static bool range_has_dirty(struct blockdev *base, uint64_t lba, uint32_t count)
{
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; i++)
        if (cache[i].valid && cache[i].dirty && cache[i].dev == base &&
            cache[i].lba >= lba && cache[i].lba < lba + count)
            return true;
    return false;
}

static bool cached_sector_read(struct blockdev *base, uint64_t lba, void *buf)
{
    struct cache_entry *e = cache_lookup(base, lba);
    if (e) {
        memcpy(buf, e->data, BLOCK_SECTOR);
        e->stamp = ++cache_clock;
        stat_hits++;
        return true;
    }
    e = cache_victim();
    if (!raw_read(base, lba, 1, e->data)) {
        e->valid = false;
        return false;
    }
    e->dev = base;
    e->lba = lba;
    e->valid = true;
    e->dirty = false;
    e->stamp = ++cache_clock;
    stat_misses++;
    memcpy(buf, e->data, BLOCK_SECTOR);
    return true;
}

bool blockdev_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > dev->sector_count)
        return false;
    uint64_t abs = lba;
    struct blockdev *base = resolve(dev, &abs);
    if (base->sector_size != BLOCK_SECTOR || !cache)
        return raw_read(base, abs, count, buf);

    mutex_lock(&cache_lock);
    bool ok;
    if (count > 1 && !range_has_dirty(base, abs, count)) {
        ok = raw_read(base, abs, count, buf);
        stat_misses += count;
    } else {
        ok = true;
        for (uint32_t i = 0; i < count && ok; i++)
            ok = cached_sector_read(base, abs + i, (uint8_t *)buf + i * BLOCK_SECTOR);
    }
    mutex_unlock(&cache_lock);
    return ok;
}

bool blockdev_read_cached(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > dev->sector_count)
        return false;
    uint64_t abs = lba;
    struct blockdev *base = resolve(dev, &abs);
    if (base->sector_size != BLOCK_SECTOR || !cache)
        return raw_read(base, abs, count, buf);
    mutex_lock(&cache_lock);
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++)
        ok = cached_sector_read(base, abs + i, (uint8_t *)buf + i * BLOCK_SECTOR);
    mutex_unlock(&cache_lock);
    return ok;
}

bool blockdev_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (lba + count > dev->sector_count)
        return false;
    uint64_t abs = lba;
    struct blockdev *base = resolve(dev, &abs);
    if (!base->write)
        return false;
    if (base->sector_size != BLOCK_SECTOR || !cache)
        return raw_write(base, abs, count, buf);

    mutex_lock(&cache_lock);
    for (uint32_t i = 0; i < count; i++) {
        struct cache_entry *e = cache_lookup(base, abs + i);
        if (!e) {
            e = cache_victim();
            e->dev = base;
            e->lba = abs + i;
            e->valid = true;
        }
        memcpy(e->data, (const uint8_t *)buf + i * BLOCK_SECTOR, BLOCK_SECTOR);
        e->dirty = true;
        e->stamp = ++cache_clock;
    }
    mutex_unlock(&cache_lock);
    return true;
}

bool blockdev_sync(void)
{
    if (!cache)
        return true;
    bool ok = true;
    mutex_lock(&cache_lock);
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; i++) {
        if (!cache[i].valid || !cache[i].dirty)
            continue;
        if (raw_write(cache[i].dev, cache[i].lba, 1, cache[i].data)) {
            cache[i].dirty = false;
            stat_writes++;
        } else {
            ok = false;
        }
    }
    mutex_unlock(&cache_lock);
    return ok;
}

void blockdev_cache_stats(uint64_t *hits, uint64_t *misses, uint64_t *writes, uint32_t *dirty)
{
    uint32_t n = 0;
    mutex_lock(&cache_lock);
    for (uint32_t i = 0; cache && i < BLOCK_CACHE_ENTRIES; i++)
        if (cache[i].valid && cache[i].dirty)
            n++;
    mutex_unlock(&cache_lock);
    *hits = stat_hits;
    *misses = stat_misses;
    *writes = stat_writes;
    *dirty = n;
}

void blockdev_cache_init(void)
{
    if (!cache)
        cache = kzalloc(sizeof(struct cache_entry) * BLOCK_CACHE_ENTRIES);
    if (!cache) {
        log_warn("block", "sem memoria para o cache de setores");
        return;
    }
    log_info("block", "cache de %u setores, write-back", BLOCK_CACHE_ENTRIES);
}

struct blockdev *blockdev_find(const char *name)
{
    for (uint32_t i = 0; i < device_count; i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return NULL;
}

struct blockdev *blockdev_at(uint32_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

uint32_t blockdev_count(void)
{
    return device_count;
}

void blockdev_scan_partitions(struct blockdev *disk)
{
    uint8_t *mbr = kmalloc(disk->sector_size);
    if (!mbr || !blockdev_read(disk, 0, 1, mbr)) {
        kfree(mbr);
        return;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        log_info("block", "%s: sem tabela de particoes MBR", disk->name);
        kfree(mbr);
        return;
    }

    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start, count;
        memcpy(&start, e + 8, 4);
        memcpy(&count, e + 12, 4);
        if (type == 0 || count == 0)
            continue;
        if ((uint64_t)start + count > disk->sector_count) {
            log_warn("block", "%s: particao %d fora do disco", disk->name, i + 1);
            continue;
        }
        char name[BLOCK_NAME_MAX];
        ksnprintf(name, sizeof(name), "%s%d", disk->name, i + 1);
        struct blockdev *p = blockdev_register(name, disk->sector_size, count, NULL, NULL, NULL);
        if (!p)
            break;
        p->parent = disk;
        p->lba_offset = start;
        p->part_type = type;
        log_info("block", "%s: LBA %u, %u setores (%u MiB), tipo 0x%02x", name, start, count,
                 (uint32_t)((uint64_t)count * disk->sector_size / MiB(1)), type);
    }
    kfree(mbr);
}

void blockdev_dump(void)
{
    kprintf("  nome    setores      tamanho    tipo\n");
    for (uint32_t i = 0; i < device_count; i++) {
        struct blockdev *d = &devices[i];
        kprintf("  %-6s  %10lu  %7lu MiB  %s%02x%s\n", d->name, d->sector_count,
                d->sector_count * d->sector_size / MiB(1), d->parent ? "particao 0x" : "disco ",
                d->parent ? d->part_type : 0, d->write || d->parent ? "" : " (somente leitura)");
    }
    uint64_t hits, misses, writes;
    uint32_t dirty;
    blockdev_cache_stats(&hits, &misses, &writes, &dirty);
    kprintf("  cache: %lu acertos, %lu faltas, %lu escritas, %u setor(es) sujo(s)\n",
            hits, misses, writes, dirty);
}
