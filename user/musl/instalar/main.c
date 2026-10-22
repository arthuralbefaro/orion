#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

#define MAX_DEVS 32
#define MNT_LOOK "/mnt/inst-olhar"

struct bdev {
    char name[16];
    char parent[16];
    char kind[8];
    char type[40];
    char model[41];
    unsigned long long sectors;
    int removable;
    char desc[96];
    bool foreign;
};

static struct bdev devs[MAX_DEVS];
static int ndevs;

static const char guid_ms_data[] = "a2a0d0ebe5b9334487c068b6b72699c7";
static const char guid_ms_reserved[] = "16e3c9e35c0bb84d817df92df00215ae";
static const char guid_ms_recovery[] = "a4bb94ded106404da16abfd50179d6ac";
static const char guid_linux_fs[] = "af3dc60f838472478e793d69d8477de4";

static int read_blockdevs(void)
{
    FILE *f = fopen("/proc/blockdevs", "r");
    if (!f) {
        fprintf(stderr, "instalar: /proc/blockdevs: %s\n", strerror(errno));
        return -1;
    }
    char line[256];
    ndevs = 0;
    while (fgets(line, sizeof(line), f) && ndevs < MAX_DEVS) {
        struct bdev *d = &devs[ndevs];
        memset(d, 0, sizeof(*d));
        char rest[128] = "";
        if (sscanf(line, "%15s %llu %15s %7s %d %39s %127[^\n]", d->name, &d->sectors, d->parent, d->kind,
                   &d->removable, d->type, rest) < 6)
            continue;
        snprintf(d->model, sizeof(d->model), "%s", rest);
        size_t n = strlen(d->model);
        while (n && (d->model[n - 1] == '\n' || d->model[n - 1] == '\r' || d->model[n - 1] == ' '))
            d->model[--n] = 0;
        ndevs++;
    }
    fclose(f);
    return 0;
}

static void look_esp(struct bdev *p)
{
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/%s", p->name);
    if (ensure_dir(MNT_LOOK) < 0 || mount(dev, MNT_LOOK, "vfat", MS_RDONLY, NULL) < 0) {
        snprintf(p->desc, sizeof(p->desc), "ESP (nao deu para olhar dentro)");
        p->foreign = true;
        return;
    }
    if (access(MNT_LOOK "/EFI/Microsoft", F_OK) == 0 || access(MNT_LOOK "/efi/Microsoft", F_OK) == 0) {
        snprintf(p->desc, sizeof(p->desc), "ESP com \\EFI\\Microsoft (carregador do Windows)");
        p->foreign = true;
    } else {
        char others[48] = "";
        const char *dirs[] = { MNT_LOOK "/EFI", MNT_LOOK "/efi" };
        for (int i = 0; i < 2; i++) {
            DIR *d = opendir(dirs[i]);
            if (!d)
                continue;
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.' || !strcasecmp(de->d_name, "BOOT") || !strcasecmp(de->d_name, "orion"))
                    continue;
                size_t len = strlen(others);
                snprintf(others + len, sizeof(others) - len, "%s%s", len ? "," : "", de->d_name);
            }
            closedir(d);
        }
        if (others[0]) {
            snprintf(p->desc, sizeof(p->desc), "ESP com outro carregador (\\EFI\\%s)", others);
            p->foreign = true;
        } else {
            snprintf(p->desc, sizeof(p->desc), "ESP sem outro carregador");
        }
    }
    umount(MNT_LOOK);
}

static void look_data(struct bdev *p)
{
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/%s", p->name);
    unsigned char sec[SECTOR * 4];
    int fd = open(dev, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, sec, sizeof(sec)) : -1;
    if (fd >= 0)
        close(fd);
    p->foreign = true;
    if (n >= SECTOR && !memcmp(sec + 3, "NTFS    ", 8))
        snprintf(p->desc, sizeof(p->desc), "NTFS (Windows)");
    else if (n >= SECTOR && !memcmp(sec + 3, "EXFAT   ", 8))
        snprintf(p->desc, sizeof(p->desc), "exFAT (dados)");
    else if (n >= SECTOR && (!memcmp(sec + 82, "FAT32   ", 8) || !memcmp(sec + 54, "FAT1", 4)))
        snprintf(p->desc, sizeof(p->desc), "FAT (dados)");
    else if (n >= 1082 && sec[1080] == 0x53 && sec[1081] == 0xEF)
        snprintf(p->desc, sizeof(p->desc), "ext2 (Linux)");
    else
        snprintf(p->desc, sizeof(p->desc), "dados Microsoft (formato nao identificado)");
}

static void describe(struct bdev *p)
{
    if (!strcmp(p->kind, "esp")) {
        look_esp(p);
    } else if (!strcmp(p->kind, "raiz")) {
        snprintf(p->desc, sizeof(p->desc), "raiz Linux x86-64 (Orion ou outro Linux instalado)");
        p->foreign = true;
    } else if (!strcmp(p->kind, "dados")) {
        snprintf(p->desc, sizeof(p->desc), "dados Linux");
        p->foreign = true;
    } else if (!strcmp(p->type, guid_ms_data)) {
        look_data(p);
    } else if (!strcmp(p->type, guid_ms_reserved)) {
        snprintf(p->desc, sizeof(p->desc), "reservada da Microsoft (Windows)");
        p->foreign = true;
    } else if (!strcmp(p->type, guid_ms_recovery)) {
        snprintf(p->desc, sizeof(p->desc), "recuperacao do Windows");
        p->foreign = true;
    } else if (!strcmp(p->type, guid_linux_fs)) {
        snprintf(p->desc, sizeof(p->desc), "sistema de arquivos Linux");
        p->foreign = true;
    } else if (!strncmp(p->type, "mbr", 3)) {
        unsigned t = (unsigned)strtoul(p->type + 3, NULL, 16);
        const char *what = t == 0x07 ? "NTFS ou exFAT (Windows)" : t == 0x27 ? "recuperacao do Windows"
                         : t == 0x83 ? "Linux" : t == 0x0B || t == 0x0C || t == 0x0E ? "FAT (dados)"
                         : t == 0xEF ? "ESP (MBR)" : "tipo MBR desconhecido";
        snprintf(p->desc, sizeof(p->desc), "%s [MBR %02x]", what, t);
        p->foreign = true;
    } else {
        snprintf(p->desc, sizeof(p->desc), "tipo desconhecido (pode ser de outro sistema)");
        p->foreign = true;
    }
}

static struct bdev *find_disk(const char *base)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].parent, "-") && !strcmp(devs[i].name, base))
            return &devs[i];
    return NULL;
}

static bool disk_foreign(const struct bdev *disk)
{
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].parent, disk->name) && devs[i].foreign)
            return true;
    return false;
}

static void print_disk(const struct bdev *disk)
{
    bool foreign = disk_foreign(disk);
    printf("  %-10s %8llu MiB  %-24s %s%s\n", disk->name, disk->sectors / 2048, disk->model,
           disk->removable ? "removivel" : "interno", foreign ? "   *** TEM OUTRO SISTEMA: nao use ***" : "");
    int parts = 0;
    for (int i = 0; i < ndevs; i++) {
        const struct bdev *p = &devs[i];
        if (strcmp(p->parent, disk->name))
            continue;
        printf("    %-12s %8llu MiB  %s%s\n", p->name, p->sectors / 2048, p->desc, p->foreign ? "  <<<" : "");
        parts++;
    }
    if (!parts)
        printf("    (sem particao reconhecida)\n");
}

static int scan_disks(void)
{
    if (read_blockdevs() < 0)
        return -1;
    for (int i = 0; i < ndevs; i++)
        if (strcmp(devs[i].parent, "-"))
            describe(&devs[i]);
    return 0;
}

static void list_disks(void)
{
    printf("Discos encontrados:\n");
    int shown = 0, clean = 0;
    for (int i = 0; i < ndevs; i++) {
        if (strcmp(devs[i].parent, "-"))
            continue;
        print_disk(&devs[i]);
        shown++;
        if (!disk_foreign(&devs[i]))
            clean++;
    }
    if (!shown)
        printf("  (nenhum)\n");
    printf("Destinos sem outro sistema: ");
    if (!clean) {
        printf("nenhum. Num disco marcado com *** o instalador so segue com confirmacao reforcada.\n");
        return;
    }
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].parent, "-") && !disk_foreign(&devs[i]))
            printf("/dev/%s ", devs[i].name);
    printf("\n");
}

static bool confirm(const char *prompt, const char *expected)
{
    printf("%s", prompt);
    fflush(stdout);
    char answer[64] = { 0 };
    if (!fgets(answer, sizeof(answer), stdin))
        return false;
    size_t n = strlen(answer);
    while (n && (answer[n - 1] == '\n' || answer[n - 1] == '\r' || answer[n - 1] == ' '))
        answer[--n] = 0;
    return strcmp(answer, expected) == 0;
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
    if (getuid() != 0) {
        fprintf(stderr, "instalar: so o root instala\n");
        return 1;
    }
    if (!dev) {
        if (scan_disks() < 0)
            return 1;
        list_disks();
        printf("uso: instalar /dev/DISCO [--sim]   (--sim so em disco sem outro sistema)\n");
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
    const char *base = strrchr(dev, '/');
    base = base ? base + 1 : dev;
    if (scan_disks() < 0)
        return 1;
    struct bdev *disk = find_disk(base);
    if (!disk) {
        fprintf(stderr, "instalar: %s nao aparece em /proc/blockdevs\n", base);
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
    bool foreign = disk_foreign(disk);
    printf("Disco escolhido:\n");
    print_disk(disk);
    if (foreign) {
        if (yes) {
            fprintf(stderr, "instalar: %s tem outro sistema; --sim nao vale aqui, confirme interativamente\n",
                    base);
            return 1;
        }
        printf("ATENCAO: %s tem particoes de outro sistema (marcadas com <<<). Instalar aqui APAGA TUDO,\n"
               "inclusive o outro sistema e os arquivos dele. Faca backup antes.\n", base);
        char prompt[128], expected[64];
        snprintf(expected, sizeof(expected), "apagar %s", base);
        snprintf(prompt, sizeof(prompt), "Para continuar mesmo assim, digite exatamente: %s\n> ", expected);
        if (!confirm(prompt, expected)) {
            printf("instalar: cancelado\n");
            return 1;
        }
    } else if (!yes) {
        char prompt[160];
        snprintf(prompt, sizeof(prompt), "Isto apaga TODO o disco %s (%llu MiB). Digite o nome do disco (%s) para continuar: ",
                 base, (unsigned long long)(size >> 20), base);
        if (!confirm(prompt, base)) {
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
    const char *mkfs_root[] = { "/bin/mkfs.ext2", "-b", "4096", "-L", "orion-raiz", rootp, NULL };
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
