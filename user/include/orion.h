#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ORION_SYS_READDIR  1000
#define ORION_SYS_OPENPTY  1001

#define FBIOGET_INFO 0x4600
#define FBIOREDRAW   0x4601

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t red_mask_size, red_mask_shift;
    uint32_t green_mask_size, green_mask_shift;
    uint32_t blue_mask_size, blue_mask_shift;
    uint64_t size;
};

static inline uint32_t fb_pack(const struct fb_info *fb, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return ((r >> (8 - fb->red_mask_size)) << fb->red_mask_shift) |
           ((g >> (8 - fb->green_mask_size)) << fb->green_mask_shift) |
           ((b >> (8 - fb->blue_mask_size)) << fb->blue_mask_shift);
}

#define INPUT_EV_KEY_DOWN     1
#define INPUT_EV_KEY_UP       2
#define INPUT_EV_MOUSE_MOVE   3
#define INPUT_EV_MOUSE_BUTTON 4

#define KEY_UP     0x148
#define KEY_DOWN   0x150
#define KEY_LEFT   0x14B
#define KEY_RIGHT  0x14D
#define KEY_HOME   0x147
#define KEY_END    0x14F
#define KEY_PGUP   0x149
#define KEY_PGDN   0x151
#define KEY_INSERT 0x152
#define KEY_DELETE 0x153

#define INPUT_MOD_SHIFT    (1 << 0)
#define INPUT_MOD_CTRL     (1 << 1)
#define INPUT_MOD_ALT      (1 << 2)
#define INPUT_MOD_CAPS     (1 << 3)
#define INPUT_MOD_EXTENDED (1 << 4)

#define INPUT_BUTTON_LEFT   (1 << 0)
#define INPUT_BUTTON_RIGHT  (1 << 1)
#define INPUT_BUTTON_MIDDLE (1 << 2)

#define INPUTIOC_SETTIMEOUT 0x4900
#define INPUTIOC_FLUSH      0x4901

struct input_event {
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
};

struct sysinfo_orion {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t shared_pages;
    uint64_t cow_faults;
    uint64_t uptime_ms;
    uint64_t heap_used;
    uint32_t cpus;
    uint32_t procs;
};

#define PROCLIST_NAME 24
#define PROCLIST_MAX  256

struct procinfo_orion {
    uint32_t pid;
    uint32_t ppid;
    uint32_t pgid;
    uint32_t state;
    uint64_t pages;
    char name[PROCLIST_NAME];
};

static inline int orion_read_text(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int len = 0;
    while (len < size - 1) {
        ssize_t n = read(fd, buf + len, (size_t)(size - 1 - len));
        if (n <= 0)
            break;
        len += (int)n;
    }
    close(fd);
    buf[len] = 0;
    return len;
}

static inline uint64_t orion_field(const char *text, const char *key)
{
    size_t klen = strlen(key);
    for (const char *line = text; line && *line;) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':')
            return strtoull(line + klen + 1, NULL, 10);
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 0;
}

static inline int orion_is_pid(const char *name)
{
    if (!*name)
        return 0;
    for (; *name; name++)
        if (*name < '0' || *name > '9')
            return 0;
    return 1;
}

static inline int orion_count_procs(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (orion_is_pid(e->d_name))
            n++;
    closedir(d);
    return n;
}

static inline long orion_free_pages(void)
{
    char buf[1024];
    if (orion_read_text("/proc/meminfo", buf, sizeof(buf)) < 0)
        return -1;
    return (long)(orion_field(buf, "MemFree") / 4);
}

static inline int orion_sysinfo(struct sysinfo_orion *out)
{
    char buf[1024];
    memset(out, 0, sizeof(*out));
    if (orion_read_text("/proc/meminfo", buf, sizeof(buf)) < 0)
        return -1;
    out->total_pages = orion_field(buf, "MemTotal") / 4;
    out->free_pages = orion_field(buf, "MemFree") / 4;
    out->used_pages = out->total_pages - out->free_pages;
    out->shared_pages = orion_field(buf, "CowShared") / 4;
    out->cow_faults = orion_field(buf, "CowFaults");
    out->heap_used = orion_field(buf, "KernelHeap") * 1024;
    if (orion_read_text("/proc/uptime", buf, sizeof(buf)) > 0) {
        char *dot;
        uint64_t sec = strtoull(buf, &dot, 10);
        uint64_t cs = *dot == '.' ? strtoull(dot + 1, NULL, 10) : 0;
        out->uptime_ms = sec * 1000 + cs * 10;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    out->cpus = cpus > 0 ? (uint32_t)cpus : 1;
    out->procs = (uint32_t)orion_count_procs();
    return 0;
}

#define ORION_PROC_SCAN 1024

static inline int orion_proclist(struct procinfo_orion *out, int max)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    uint32_t *pids = malloc(sizeof(uint32_t) * ORION_PROC_SCAN);
    if (!pids) {
        closedir(d);
        return -1;
    }
    int total = 0;
    struct dirent *e;
    while ((e = readdir(d)) && total < ORION_PROC_SCAN)
        if (orion_is_pid(e->d_name))
            pids[total++] = (uint32_t)strtoul(e->d_name, NULL, 10);
    closedir(d);
    int n = 0;
    char path[64], buf[512];
    for (int i = total - 1; i >= 0 && n < max; i--) {
        snprintf(path, sizeof(path), "/proc/%u/status", pids[i]);
        if (orion_read_text(path, buf, sizeof(buf)) <= 0)
            continue;
        struct procinfo_orion *p = &out[n];
        memset(p, 0, sizeof(*p));
        p->pid = (uint32_t)orion_field(buf, "Pid");
        p->ppid = (uint32_t)orion_field(buf, "PPid");
        p->pgid = (uint32_t)orion_field(buf, "Pgid");
        p->pages = orion_field(buf, "VmRSS") / 4;
        const char *st = strstr(buf, "State:");
        p->state = st && strchr(st, 'Z') && strchr(st, 'Z') < strchr(st, '\n') ? 1 : 0;
        const char *nm = strstr(buf, "Name:");
        if (nm) {
            nm += 5;
            while (*nm == '\t' || *nm == ' ')
                nm++;
            size_t len = strcspn(nm, "\n");
            if (len >= PROCLIST_NAME)
                len = PROCLIST_NAME - 1;
            memcpy(p->name, nm, len);
            p->name[len] = 0;
        }
        n++;
    }
    free(pids);
    return n;
}

static inline int orion_openpty(int *master, int *slave)
{
    int fds[2];
    if (syscall(ORION_SYS_OPENPTY, fds) < 0)
        return -1;
    *master = fds[0];
    *slave = fds[1];
    return 0;
}

#define SIOCGIFCONF_ORION 0x8900
#define SIOCSIFCONF_ORION 0x8901
#define SIOCDHCP_ORION    0x8902

struct ifreq_orion {
    uint8_t mac[6];
    uint8_t link_up;
    uint8_t pad;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t irqs;
};

static inline const char *orion_strerror(int err)
{
    static const char *const msgs[] = {
        [0] = "Sucesso", [EPERM] = "Operacao nao permitida", [ENOENT] = "Arquivo ou diretorio inexistente",
        [ESRCH] = "Processo inexistente", [ENOEXEC] = "Formato de executavel invalido",
        [EBADF] = "Descritor de arquivo invalido", [ECHILD] = "Sem processos filhos",
        [EAGAIN] = "Tente novamente", [ENOMEM] = "Sem memoria", [EACCES] = "Permissao negada",
        [EFAULT] = "Endereco invalido", [ENOTDIR] = "Nao e um diretorio", [EISDIR] = "E um diretorio",
        [EINVAL] = "Argumento invalido", [EMFILE] = "Muitos arquivos abertos", [ENOTTY] = "Nao e um terminal",
        [ESPIPE] = "Seek invalido", [EPIPE] = "Pipe quebrado", [ERANGE] = "Fora do intervalo",
        [ENOSYS] = "Funcao nao implementada", [EIO] = "Erro de entrada/saida",
        [EEXIST] = "Arquivo ja existe", [ENOTEMPTY] = "Diretorio nao vazio",
        [EROFS] = "Sistema de arquivos somente leitura", [EINTR] = "Chamada interrompida",
        [ENAMETOOLONG] = "Nome muito longo", [EBUSY] = "Recurso ocupado",
        [EXDEV] = "Link entre dispositivos diferentes", [ENOTBLK] = "Nao e um dispositivo de bloco",
        [ENODEV] = "Dispositivo ou tipo de sistema de arquivos desconhecido", [ENOSPC] = "Sem espaco no dispositivo",
        [EFBIG] = "Arquivo grande demais",
    };
    if (err >= 0 && err < (int)(sizeof(msgs) / sizeof(msgs[0])) && msgs[err])
        return msgs[err];
    return "Erro desconhecido";
}

static inline void orion_perror(const char *what)
{
    int err = errno;
    if (what && *what)
        fprintf(stderr, "%s: %s\n", what, orion_strerror(err));
    else
        fprintf(stderr, "%s\n", orion_strerror(err));
}

static inline int orion_inet_aton(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return 0;
            s++;
        }
        ip = (ip << 8) | v;
        if (part < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    if (*s)
        return 0;
    *out = ip;
    return 1;
}

static inline char *orion_inet_ntoa(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}
