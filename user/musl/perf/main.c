#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <orion.h>

#define PROF_START 1
#define PROF_STOP 2
#define PROF_READ 3
#define PROF_STATS 4
#define PROF_SPIN_IRQOFF 5
#define MAX_SAMPLES (16384 * 32)
#define TOP 15

struct sample {
    uint64_t rip;
    uint32_t pid;
    uint16_t cpu;
    uint8_t user;
    uint8_t irq_off;
};

struct stats {
    uint64_t samples;
    uint64_t dropped;
    uint64_t elapsed_ms;
    uint32_t hz;
    uint32_t cpus;
};

struct sym {
    uint64_t addr;
    uint64_t size;
    const char *name;
};

struct bucket {
    char name[64];
    uint64_t count;
    uint64_t irq_off;
};

static struct sym *syms;
static size_t nsyms;
static char *strtab_copy;

static int by_addr(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int load_symbols(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    uint8_t *img = malloc((size_t)st.st_size);
    if (!img || read(fd, img, (size_t)st.st_size) != st.st_size) {
        free(img);
        close(fd);
        return -1;
    }
    close(fd);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_shoff == 0) {
        free(img);
        return -1;
    }
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB)
            continue;
        Elf64_Sym *s = (Elf64_Sym *)(img + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
        Elf64_Shdr *str = &sh[sh[i].sh_link];
        strtab_copy = malloc(str->sh_size);
        syms = calloc(n, sizeof(*syms));
        if (!strtab_copy || !syms)
            break;
        memcpy(strtab_copy, img + str->sh_offset, str->sh_size);
        for (size_t k = 0; k < n; k++) {
            if (ELF64_ST_TYPE(s[k].st_info) != STT_FUNC || !s[k].st_value)
                continue;
            syms[nsyms].addr = s[k].st_value;
            syms[nsyms].size = s[k].st_size;
            syms[nsyms].name = strtab_copy + s[k].st_name;
            nsyms++;
        }
        break;
    }
    free(img);
    qsort(syms, nsyms, sizeof(*syms), by_addr);
    return nsyms ? 0 : -1;
}

static const char *kernel_name(uint64_t rip)
{
    size_t lo = 0, hi = nsyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (syms[mid].addr <= rip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return "[kernel ?]";
    struct sym *s = &syms[lo - 1];
    if (s->size && rip >= s->addr + s->size)
        return "[kernel ?]";
    return s->name;
}

static void proc_name(uint32_t pid, char *out, size_t cap)
{
    char path[64], line[128] = "";
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
    }
    char *a = strchr(line, '('), *b = strrchr(line, ')');
    if (a && b && b > a + 1) {
        *b = 0;
        snprintf(out, cap, "[usuario] %s (pid %u)", a + 1, pid);
    } else {
        snprintf(out, cap, "[usuario] pid %u", pid);
    }
}

static struct bucket *buckets;
static size_t nbuckets;

static void add(const char *name, int irq_off)
{
    for (size_t i = 0; i < nbuckets; i++) {
        if (!strcmp(buckets[i].name, name)) {
            buckets[i].count++;
            buckets[i].irq_off += irq_off;
            return;
        }
    }
    struct bucket *nb = realloc(buckets, (nbuckets + 1) * sizeof(*buckets));
    if (!nb)
        return;
    buckets = nb;
    snprintf(buckets[nbuckets].name, sizeof(buckets[nbuckets].name), "%s", name);
    buckets[nbuckets].count = 1;
    buckets[nbuckets].irq_off = irq_off;
    nbuckets++;
}

static int by_count(const void *a, const void *b)
{
    const struct bucket *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void usage(void)
{
    fprintf(stderr, "uso: perf [-f hz] [-t segundos] [-z segundos] [-i ms] [comando args...]\n"
                    "  -f hz   amostras por segundo em cada CPU (padrao 1000)\n"
                    "  -t s    mede por s segundos sem comando (padrao 2)\n"
                    "  -z s    carga de teste: le /dev/zero por s segundos\n"
                    "  -i ms   teste: o kernel gira ms com interrupcoes desligadas (modo de teste)\n");
}

static void zero_load(int secs)
{
    int fd = open("/dev/zero", O_RDONLY);
    static char buf[65536];
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        if (read(fd, buf, sizeof(buf)) < 0)
            break;
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < secs);
    _exit(0);
}

int main(int argc, char **argv)
{
    long freq = 1000, secs = 2, zero = 0, irqoff = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (!strcmp(argv[i], "-f"))
            freq = atol(argv[++i]);
        else if (!strcmp(argv[i], "-t"))
            secs = atol(argv[++i]);
        else if (!strcmp(argv[i], "-z"))
            zero = atol(argv[++i]);
        else if (!strcmp(argv[i], "-i"))
            irqoff = atol(argv[++i]);
        else {
            usage();
            return 2;
        }
    }
    const char *paths[] = { "/boot/efi/boot/orion.elf", "/boot/orion.elf" };
    const char *symfile = NULL;
    for (size_t k = 0; k < sizeof(paths) / sizeof(paths[0]) && !symfile; k++)
        if (load_symbols(paths[k]) == 0)
            symfile = paths[k];
    if (syscall(ORION_SYS_PROF, PROF_START, freq, 0, 0) < 0) {
        fprintf(stderr, "perf: nao consegui ligar a amostragem: %s\n", strerror(errno));
        return 1;
    }
    if (irqoff && syscall(ORION_SYS_PROF, PROF_SPIN_IRQOFF, irqoff, 0, 0) < 0)
        fprintf(stderr, "perf: teste com interrupcoes desligadas: %s\n", strerror(errno));
    if (zero || i < argc) {
        pid_t pid = fork();
        if (pid == 0) {
            if (zero)
                zero_load((int)zero);
            execvp(argv[i], argv + i);
            fprintf(stderr, "perf: %s: %s\n", argv[i], strerror(errno));
            _exit(127);
        }
        int st = 0;
        if (pid > 0)
            waitpid(pid, &st, 0);
    } else if (!irqoff) {
        sleep((unsigned)secs);
    }
    syscall(ORION_SYS_PROF, PROF_STOP, 0, 0, 0);
    struct stats st;
    memset(&st, 0, sizeof(st));
    syscall(ORION_SYS_PROF, PROF_STATS, 0, &st, 0);
    struct sample *s = malloc(MAX_SAMPLES * sizeof(*s));
    long n = s ? syscall(ORION_SYS_PROF, PROF_READ, 0, s, MAX_SAMPLES) : -1;
    if (n < 0) {
        fprintf(stderr, "perf: nao consegui ler as amostras: %s\n", strerror(errno));
        return 1;
    }
    uint64_t user = 0, off = 0;
    for (long k = 0; k < n; k++) {
        char name[64];
        if (s[k].user) {
            user++;
            proc_name(s[k].pid, name, sizeof(name));
        } else {
            snprintf(name, sizeof(name), "%s", symfile ? kernel_name(s[k].rip) : "[kernel]");
        }
        off += s[k].irq_off;
        add(name, s[k].irq_off);
    }
    qsort(buckets, nbuckets, sizeof(*buckets), by_count);
    printf("perf: %ld amostra(s) em %llu ms, %u Hz em %u CPU(s), %llu perdida(s); simbolos de %s\n", n,
           (unsigned long long)st.elapsed_ms, st.hz, st.cpus, (unsigned long long)st.dropped,
           symfile ? symfile : "(nenhum)");
    if (n > 0)
        printf("perf: %.1f%% no usuario, %.1f%% no kernel, %.1f%% com interrupcoes desligadas\n", 100.0 * user / n,
               100.0 * (n - user) / n, 100.0 * off / n);
    printf("perf:   %%     amostras  sem IRQ  funcao\n");
    for (size_t k = 0; k < nbuckets && k < TOP; k++)
        printf("perf: %5.1f%%  %8llu  %7llu  %s\n", n ? 100.0 * buckets[k].count / n : 0.0,
               (unsigned long long)buckets[k].count, (unsigned long long)buckets[k].irq_off, buckets[k].name);
    printf("perf: fim\n");
    return 0;
}
