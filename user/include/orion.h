#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ORION_SYS_READDIR  1000
#define ORION_SYS_OPENPTY  1001
#define ORION_SYS_SYSINFO  1002
#define ORION_SYS_PROCLIST 1003

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

static inline int sysinfo_orion(struct sysinfo_orion *out)
{
    return (int)syscall(ORION_SYS_SYSINFO, out);
}

static inline int proclist_orion(struct procinfo_orion *out, int max)
{
    return (int)syscall(ORION_SYS_PROCLIST, out, max);
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
