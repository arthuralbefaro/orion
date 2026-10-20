#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SECTOR 512
#define ENTRIES 128
#define ENTRY_SIZE 128
#define ESP_FIRST 2048
#define ESP_SECTORS (64u * 1024 * 1024 / SECTOR)
#define MIN_BYTES (256ull * 1024 * 1024)
#define PAYLOAD "/usr/lib/instalar/esp"
#define MNT_EFI "/mnt/inst-efi"
#define MNT_ROOT "/mnt/inst-raiz"

static const uint8_t type_esp[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                      0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t type_root[16] = { 0xE3, 0xBC, 0x68, 0x4F, 0xCD, 0xE8, 0xB1, 0x4D,
                                       0x96, 0xE7, 0xFB, 0xCA, 0xF9, 0x84, 0xB7, 0x09 };

static uint32_t crc_table[256];
static unsigned long files, dirs, links;
static unsigned long long bytes;
static dev_t root_dev;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void random_guid(uint8_t *g)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, g, 16) != 16) {
        for (int i = 0; i < 16; i++)
            g[i] = (uint8_t)(rand() & 0xFF);
    }
    if (fd >= 0)
        close(fd);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

static void put_name(uint8_t *p, const char *name)
{
    for (int i = 0; name[i] && i < 36; i++) {
        p[i * 2] = (uint8_t)name[i];
        p[i * 2 + 1] = 0;
    }
}

static int write_at(int fd, uint64_t lba, const void *buf, size_t len)
{
    if (lseek(fd, (off_t)(lba * SECTOR), SEEK_SET) != (off_t)(lba * SECTOR))
        return -1;
    return write(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

static void header(uint8_t *h, uint64_t my, uint64_t alt, uint64_t last, const uint8_t *guid, uint64_t entries_lba,
                   uint32_t entries_crc)
{
    memset(h, 0, SECTOR);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, 34);
    put64(h + 48, last);
    memcpy(h + 56, guid, 16);
    put64(h + 72, entries_lba);
    put32(h + 80, ENTRIES);
    put32(h + 84, ENTRY_SIZE);
    put32(h + 88, entries_crc);
    put32(h + 16, crc32(h, 92));
}

static int partition(const char *dev, uint64_t sectors, uint64_t *root_first, uint64_t *root_last)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "instalar: %s: %s\n", dev, strerror(errno));
        return -1;
    }
    uint64_t last = sectors - 34;
    *root_first = ESP_FIRST + ESP_SECTORS;
    *root_last = last;

    uint8_t mbr[SECTOR];
    memset(mbr, 0, sizeof(mbr));
    uint8_t *e = mbr + 446;
    e[1] = 0x00;
    e[2] = 0x02;
    e[4] = 0xEE;
    e[5] = e[6] = e[7] = 0xFF;
    put32(e + 8, 1);
    put32(e + 12, sectors - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(sectors - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    static uint8_t entries[ENTRIES * ENTRY_SIZE];
    memset(entries, 0, sizeof(entries));
    memcpy(entries, type_esp, 16);
    random_guid(entries + 16);
    put64(entries + 32, ESP_FIRST);
    put64(entries + 40, ESP_FIRST + ESP_SECTORS - 1);
    put_name(entries + 56, "EFI System");
    uint8_t *r = entries + ENTRY_SIZE;
    memcpy(r, type_root, 16);
    random_guid(r + 16);
    put64(r + 32, *root_first);
    put64(r + 40, *root_last);
    put_name(r + 56, "orion-raiz");
    uint32_t ecrc = crc32(entries, sizeof(entries));

    uint8_t guid[16];
    random_guid(guid);
    uint8_t primary[SECTOR], backup[SECTOR];
    header(primary, 1, sectors - 1, last, guid, 2, ecrc);
    header(backup, sectors - 1, 1, last, guid, sectors - 33, ecrc);

    int bad = write_at(fd, 0, mbr, SECTOR) || write_at(fd, 1, primary, SECTOR) ||
              write_at(fd, 2, entries, sizeof(entries)) || write_at(fd, sectors - 33, entries, sizeof(entries)) ||
              write_at(fd, sectors - 1, backup, SECTOR);
    if (bad) {
        fprintf(stderr, "instalar: escrita da GPT em %s falhou: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    fsync(fd);
    sync();
    if (ioctl(fd, BLKRRPART, 0) < 0) {
        fprintf(stderr, "instalar: o kernel nao releu as particoes de %s: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int run(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int copy_file(const char *from, const char *to, const struct stat *st)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777);
    if (out < 0) {
        close(in);
        return -1;
    }
    static char buf[65536];
    ssize_t n;
    int bad = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            bad = 1;
            break;
        }
        bytes += (unsigned long long)n;
    }
    if (n < 0)
        bad = 1;
    close(in);
    if (fchown(out, st->st_uid, st->st_gid) == 0)
        fchmod(out, st->st_mode & 07777);
    close(out);
    files++;
    return bad ? -1 : 0;
}

static mode_t mount_point_mode(const char *name)
{
    if (!strcmp(name, "tmp"))
        return 01777;
    if (!strcmp(name, "proc"))
        return 0555;
    return 0755;
}

static int copy_tree(const char *from, const char *to, bool top, bool keep_owner)
{
    DIR *d = opendir(from);
    if (!d) {
        fprintf(stderr, "instalar: %s: %s\n", from, strerror(errno));
        return -1;
    }
    int bad = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", strcmp(from, "/") ? from : "", de->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, de->d_name);
        if (top && (!strcmp(de->d_name, "lost+found") || !strcmp(src, "/usr/lib/instalar")))
            continue;
        struct stat st;
        if (lstat(src, &st) < 0) {
            bad = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            bool other = keep_owner && st.st_dev != root_dev;
            mode_t mode = other ? mount_point_mode(de->d_name) : (st.st_mode & 07777);
            if (mkdir(dst, mode) < 0 && errno != EEXIST) {
                fprintf(stderr, "instalar: mkdir %s: %s\n", dst, strerror(errno));
                bad = 1;
                continue;
            }
            if (keep_owner && !other && lchown(dst, st.st_uid, st.st_gid) == 0)
                chmod(dst, st.st_mode & 07777);
            else if (other)
                chmod(dst, mode);
            dirs++;
            if (!other && copy_tree(src, dst, false, keep_owner) < 0)
                bad = 1;
        } else if (S_ISLNK(st.st_mode)) {
            char target[1024];
            ssize_t n = readlink(src, target, sizeof(target) - 1);
            if (n < 0 || (target[n] = 0, symlink(target, dst) < 0)) {
                bad = 1;
                continue;
            }
            if (keep_owner)
                lchown(dst, st.st_uid, st.st_gid);
            links++;
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file(src, dst, &st) < 0) {
                fprintf(stderr, "instalar: copia de %s falhou: %s\n", src, strerror(errno));
                bad = 1;
            }
        }
    }
    closedir(d);
    return bad ? -1 : 0;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_partition(const char *dev)
{
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    size_t len = strlen(base);
    if (!len || !is_digit(base[len - 1]))
        return false;
    if (!strncmp(base, "nvme", 4)) {
        const char *pp = strrchr(base, 'p');
        const char *nn = strrchr(base, 'n');
        return pp && nn && pp > nn;
    }
    return true;
}

static void part_name(char *out, size_t cap, const char *dev, int n)
{
    size_t len = strlen(dev);
    snprintf(out, cap, "%s%s%d", dev, len && is_digit(dev[len - 1]) ? "p" : "", n);
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "instalar: mkdir %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = NULL;
    bool yes = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim"))
            yes = true;
        else if (!dev)
            dev = argv[i];
    }
    if (!dev) {
        fprintf(stderr, "uso: instalar /dev/sdX [--sim]\n");
        return 2;
    }
    if (is_partition(dev)) {
        fprintf(stderr, "instalar: %s parece uma particao; use o disco inteiro\n", dev);
        return 1;
    }
    struct stat st;
    if (stat(dev, &st) < 0 || !S_ISBLK(st.st_mode)) {
        fprintf(stderr, "instalar: %s nao e um disco\n", dev);
        return 1;
    }
    if (access(PAYLOAD "/boot/orion.elf", R_OK) < 0) {
        fprintf(stderr, "instalar: sem %s; rode o instalador pelo sistema live da ISO\n", PAYLOAD);
        return 1;
    }
    int fd = open(dev, O_RDONLY);
    off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0)
        close(fd);
    if (size < (off_t)MIN_BYTES) {
        fprintf(stderr, "instalar: %s tem %lld MiB; precisa de pelo menos %llu MiB\n", dev,
                (long long)(size > 0 ? size >> 20 : 0), MIN_BYTES >> 20);
        return 1;
    }
    uint64_t sectors = (uint64_t)size / SECTOR;
    if (!yes) {
        printf("Isto apaga TODO o disco %s (%llu MiB). Digite SIM para continuar: ", dev,
               (unsigned long long)(size >> 20));
        fflush(stdout);
        char answer[16] = { 0 };
        if (!fgets(answer, sizeof(answer), stdin) || strncmp(answer, "SIM", 3) != 0) {
            printf("instalar: cancelado\n");
            return 1;
        }
    }
    crc_init();
    printf("instalar: %s, %llu MiB: GPT com particao EFI de 64 MiB e raiz ext2 no resto\n", dev,
           (unsigned long long)(size >> 20));
    uint64_t root_first, root_last;
    if (partition(dev, sectors, &root_first, &root_last) < 0)
        return 1;
    char esp[64], rootp[64];
    part_name(esp, sizeof(esp), dev, 1);
    part_name(rootp, sizeof(rootp), dev, 2);
    if (access(esp, F_OK) < 0 || access(rootp, F_OK) < 0) {
        fprintf(stderr, "instalar: as particoes novas nao apareceram (%s, %s)\n", esp, rootp);
        return 1;
    }
    printf("instalar: particoes %s e %s criadas; formatando\n", esp, rootp);
    const char *mkfs_esp[] = { "/bin/mkfs", esp, "ORION-EFI", NULL };
    const char *mkfs_root[] = { "/bin/mkfs.ext2", "-L", "orion-raiz", rootp, NULL };
    if (run(mkfs_esp) < 0 || run(mkfs_root) < 0) {
        fprintf(stderr, "instalar: a formatacao falhou\n");
        return 1;
    }
    if (ensure_dir(MNT_EFI) < 0 || ensure_dir(MNT_ROOT) < 0)
        return 1;
    if (mount(esp, MNT_EFI, "vfat", 0, NULL) < 0 || mount(rootp, MNT_ROOT, "ext2", 0, NULL) < 0) {
        fprintf(stderr, "instalar: mount falhou: %s\n", strerror(errno));
        return 1;
    }
    printf("instalar: copiando o carregador e o kernel para a particao EFI\n");
    int bad = copy_tree(PAYLOAD, MNT_EFI, false, false);
    struct stat rs;
    stat("/", &rs);
    root_dev = rs.st_dev;
    printf("instalar: copiando o sistema para a raiz\n");
    bad |= copy_tree("/", MNT_ROOT, true, true);
    sync();
    umount(MNT_EFI);
    umount(MNT_ROOT);
    sync();
    if (bad) {
        fprintf(stderr, "instalar: houve erros na copia; o disco pode nao bootar\n");
        return 1;
    }
    printf("instalar: pronto: %lu arquivo(s), %lu diretorio(s), %lu link(s), %llu MiB copiados\n", files, dirs, links,
           bytes >> 20);
    printf("instalar: tire a ISO e reinicie para bootar do disco (so UEFI)\n");
    return 0;
}
