#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orion.h>

#define SECTOR     512
#define RESERVED   1
#define NFATS      2
#define ROOT_ENTS  512

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: mkfs <dispositivo> [rotulo]\n");
        fprintf(stderr, "     formata em FAT16 (ex.: mkfs /dev/sdb ORION)\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *label = argc > 2 ? argv[2] : "ORION";

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "mkfs: %s: %s\n", argv[1], orion_strerror(errno));
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 1024 * 1024) {
        fprintf(stderr, "mkfs: %s: tamanho invalido\n", argv[1]);
        return 1;
    }

    uint32_t total = (uint32_t)(st.st_size / SECTOR);
    uint32_t spc = 4;
    while (total / spc > 60000 && spc < 64)
        spc *= 2;

    uint32_t root_sectors = (ROOT_ENTS * 32 + SECTOR - 1) / SECTOR;
    uint32_t fat_sectors = 1;
    for (int i = 0; i < 8; i++) {
        uint32_t data = total - RESERVED - NFATS * fat_sectors - root_sectors;
        uint32_t clusters = data / spc;
        fat_sectors = ((clusters + 2) * 2 + SECTOR - 1) / SECTOR;
    }
    uint32_t data_start = RESERVED + NFATS * fat_sectors + root_sectors;
    uint32_t clusters = (total - data_start) / spc;

    printf("mkfs: %s, %u setores, %u setores/cluster, %u clusters, FAT de %u setores\n",
           argv[1], total, spc, clusters, fat_sectors);
    if (clusters < 4085 || clusters >= 65525) {
        fprintf(stderr, "mkfs: numero de clusters (%u) fora da faixa do FAT16\n", clusters);
        return 1;
    }

    uint8_t *sector = calloc(1, SECTOR);
    if (!sector) {
        fprintf(stderr, "mkfs: sem memoria\n");
        return 1;
    }

    sector[0] = 0xEB;
    sector[1] = 0x3C;
    sector[2] = 0x90;
    memcpy(sector + 3, "ORION1.0", 8);
    put16(sector + 11, SECTOR);
    sector[13] = (uint8_t)spc;
    put16(sector + 14, RESERVED);
    sector[16] = NFATS;
    put16(sector + 17, ROOT_ENTS);
    put16(sector + 19, total > 0xFFFF ? 0 : (uint16_t)total);
    sector[21] = 0xF8;
    put16(sector + 22, (uint16_t)fat_sectors);
    put16(sector + 24, 63);
    put16(sector + 26, 255);
    put32(sector + 28, 0);
    put32(sector + 32, total > 0xFFFF ? total : 0);
    sector[36] = 0x80;
    sector[38] = 0x29;
    put32(sector + 39, 0x4F52494F);
    memset(sector + 43, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++)
        sector[43 + i] = (uint8_t)(label[i] >= 'a' && label[i] <= 'z' ? label[i] - 32 : label[i]);
    memcpy(sector + 54, "FAT16   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (write(fd, sector, SECTOR) != SECTOR) {
        fprintf(stderr, "mkfs: erro escrevendo o setor de boot\n");
        return 1;
    }

    memset(sector, 0, SECTOR);
    sector[0] = 0xF8;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    sector[3] = 0xFF;
    for (uint32_t copy = 0; copy < NFATS; copy++) {
        lseek(fd, (long)(RESERVED + copy * fat_sectors) * SECTOR, SEEK_SET);
        if (write(fd, sector, SECTOR) != SECTOR) {
            fprintf(stderr, "mkfs: erro escrevendo a FAT\n");
            return 1;
        }
        memset(sector, 0, 4);
        for (uint32_t s = 1; s < fat_sectors; s++)
            if (write(fd, sector, SECTOR) != SECTOR) {
                fprintf(stderr, "mkfs: erro escrevendo a FAT\n");
                return 1;
            }
        sector[0] = 0xF8;
        sector[1] = 0xFF;
        sector[2] = 0xFF;
        sector[3] = 0xFF;
    }

    memset(sector, 0, SECTOR);
    lseek(fd, (long)(RESERVED + NFATS * fat_sectors) * SECTOR, SEEK_SET);
    for (uint32_t s = 0; s < root_sectors; s++)
        if (write(fd, sector, SECTOR) != SECTOR) {
            fprintf(stderr, "mkfs: erro limpando o diretorio raiz\n");
            return 1;
        }

    free(sector);
    fsync(fd);
    close(fd);
    sync();
    printf("mkfs: pronto. Use 'mount' no shell do kernel (Ctrl+K) para montar em /mnt.\n");
    return 0;
}
