#include <fs/block.h>
#include <kernel/log.h>
#include <mm/heap.h>
#include <lib/string.h>

static struct blockdev devices[BLOCK_MAX_DEVICES];
static uint32_t device_count;

struct blockdev *blockdev_register(const char *name, uint32_t sector_size, uint64_t sector_count,
                                   blockdev_read_fn read, void *priv)
{
    if (device_count >= BLOCK_MAX_DEVICES)
        return NULL;
    struct blockdev *d = &devices[device_count++];
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    d->sector_size = sector_size;
    d->sector_count = sector_count;
    d->read = read;
    d->priv = priv;
    return d;
}

bool blockdev_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (lba + count > dev->sector_count)
        return false;
    if (dev->parent)
        return blockdev_read(dev->parent, dev->lba_offset + lba, count, buf);
    return dev->read(dev, lba, count, buf);
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
        struct blockdev *p = blockdev_register(name, disk->sector_size, count, NULL, NULL);
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
        kprintf("  %-6s  %10lu  %7lu MiB  %s%02x\n", d->name, d->sector_count,
                d->sector_count * d->sector_size / MiB(1), d->parent ? "particao 0x" : "disco ",
                d->parent ? d->part_type : 0);
    }
}
