#include <fs/block.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/mutex.h>
#include <mm/heap.h>
#include <lib/string.h>
#include <lib/crc32.h>

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

bool blockdev_writable(const struct blockdev *dev)
{
    while (dev->parent)
        dev = dev->parent;
    return dev->write != NULL;
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

#define GPT_MAX_ENTRY_BYTES (256u * 1024)

struct gpt_header {
    char sig[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
} PACKED;

struct gpt_entry {
    uint8_t type[16];
    uint8_t unique[16];
    uint64_t first;
    uint64_t last;
    uint64_t attrs;
    uint16_t name[36];
} PACKED;

static const uint8_t guid_esp[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                      0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t guid_root[16] = { 0xE3, 0xBC, 0x68, 0x4F, 0xCD, 0xE8, 0xB1, 0x4D,
                                       0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09 };
static const uint8_t guid_data[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                       0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
static const uint8_t guid_zero[16];

static uint8_t guid_kind(const uint8_t *g)
{
    if (memcmp(g, guid_esp, 16) == 0)
        return PART_ESP;
    if (memcmp(g, guid_root, 16) == 0)
        return PART_ROOT;
    if (memcmp(g, guid_data, 16) == 0)
        return PART_DATA;
    return PART_OTHER;
}

static uint8_t *gpt_load(struct blockdev *disk, uint64_t lba, struct gpt_header *h)
{
    uint32_t ss = disk->sector_size;
    uint8_t *sec = kmalloc(ss);
    if (!sec)
        return NULL;
    bool ok = blockdev_read(disk, lba, 1, sec);
    if (ok) {
        memcpy(h, sec, sizeof(*h));
        ok = memcmp(h->sig, "EFI PART", 8) == 0 && h->header_size >= sizeof(*h) && h->header_size <= ss &&
             h->my_lba == lba;
    }
    if (ok) {
        memset(sec + 16, 0, 4);
        ok = crc32(sec, h->header_size) == h->header_crc;
    }
    kfree(sec);
    if (!ok)
        return NULL;
    uint64_t bytes = (uint64_t)h->entry_count * h->entry_size;
    if (h->entry_size < sizeof(struct gpt_entry) || h->entry_size % 8 || h->entry_count == 0 ||
        bytes > GPT_MAX_ENTRY_BYTES)
        return NULL;
    uint64_t sectors = (bytes + ss - 1) / ss;
    if (h->first_usable > h->last_usable || h->last_usable >= disk->sector_count ||
        h->entries_lba < 2 || h->entries_lba + sectors > disk->sector_count)
        return NULL;
    uint8_t *ents = kmalloc(sectors * ss);
    if (!ents)
        return NULL;
    if (!blockdev_read(disk, h->entries_lba, (uint32_t)sectors, ents) || crc32(ents, bytes) != h->entries_crc) {
        kfree(ents);
        return NULL;
    }
    return ents;
}

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case PART_ESP: return "ESP";
    case PART_ROOT: return "raiz Linux x86-64";
    case PART_DATA: return "dados Linux";
    default: return "outro tipo";
    }
}

static void part_name(const struct blockdev *disk, uint32_t index, char out[BLOCK_NAME_MAX])
{
    size_t len = strlen(disk->name);
    bool digit = len && disk->name[len - 1] >= '0' && disk->name[len - 1] <= '9';
    ksnprintf(out, BLOCK_NAME_MAX, "%s%s%u", disk->name, digit ? "p" : "", index);
}

static void scan_gpt(struct blockdev *disk)
{
    struct gpt_header h;
    uint8_t *ents = gpt_load(disk, 1, &h);
    if (!ents) {
        ents = gpt_load(disk, disk->sector_count - 1, &h);
        if (!ents) {
            log_warn("block", "%s: GPT invalida (cabecalho e copia de reserva)", disk->name);
            return;
        }
        log_warn("block", "%s: GPT principal invalida, usando a copia do fim do disco", disk->name);
    }
    for (uint32_t i = 0; i < h.entry_count; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(ents + (uint64_t)i * h.entry_size);
        if (memcmp(e->type, guid_zero, 16) == 0)
            continue;
        if (e->first > e->last || e->first < h.first_usable || e->last > h.last_usable) {
            log_warn("block", "%s: particao GPT %u fora da area utilizavel", disk->name, i + 1);
            continue;
        }
        char name[BLOCK_NAME_MAX];
        part_name(disk, i + 1, name);
        uint64_t count = e->last - e->first + 1;
        struct blockdev *p = blockdev_register(name, disk->sector_size, count, NULL, NULL, NULL);
        if (!p)
            break;
        p->parent = disk;
        p->lba_offset = e->first;
        p->part_type = PART_GPT;
        p->part_kind = guid_kind(e->type);
        memcpy(p->type_guid, e->type, 16);
        log_info("block", "%s: GPT, LBA %lu, %lu setores (%lu MiB), %s", name, e->first, count,
                 count * disk->sector_size / MiB(1), kind_name(p->part_kind));
    }
    kfree(ents);
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
        if (mbr[446 + i * 16 + 4] == PART_GPT) {
            kfree(mbr);
            scan_gpt(disk);
            return;
        }
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
        part_name(disk, (uint32_t)i + 1, name);
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
        if (d->parent && d->part_type == PART_GPT) {
            kprintf("  %-6s  %10lu  %7lu MiB  gpt %s\n", d->name, d->sector_count,
                    d->sector_count * d->sector_size / MiB(1), kind_name(d->part_kind));
            continue;
        }
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
