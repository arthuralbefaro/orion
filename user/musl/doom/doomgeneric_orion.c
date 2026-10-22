#include "orion_sound.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "orion_dev.h"

#define KEY_QUEUE 64

struct key_event {
    unsigned char key;
    int pressed;
};

static int fb_fd = -1;
static int in_fd = -1;
static struct orion_fb_info fbi;
static uint8_t *fb;
static uint32_t scale = 1;
static uint32_t off_x;
static uint32_t off_y;
static uint32_t *row;
static uint64_t start_ms;
static uint64_t quit_after_ms;
static unsigned long frames;
static struct key_event keyq[KEY_QUEUE];
static unsigned kq_head;
static unsigned kq_tail;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t channel(uint32_t v, uint32_t size, uint32_t shift)
{
    v = size <= 8 ? v >> (8 - size) : v << (size - 8);
    return v << shift;
}

static uint32_t pack(uint32_t rgb)
{
    return channel((rgb >> 16) & 0xFF, fbi.red_mask_size, fbi.red_mask_shift) |
           channel((rgb >> 8) & 0xFF, fbi.green_mask_size, fbi.green_mask_shift) |
           channel(rgb & 0xFF, fbi.blue_mask_size, fbi.blue_mask_shift);
}

static void die(const char *what)
{
    fprintf(stderr, "doom: %s: %s\n", what, strerror(errno));
    exit(1);
}

void DG_Init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0)
        die("/dev/fb0");
    if (ioctl(fb_fd, ORION_FBIOGET_INFO, &fbi) < 0)
        die("FBIOGET_INFO");
    if (fbi.bpp != 32) {
        fprintf(stderr, "doom: framebuffer de %u bits; so 32 e suportado\n", fbi.bpp);
        exit(1);
    }
    fb = mmap(NULL, fbi.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED)
        die("mmap do framebuffer");
    uint32_t sx = fbi.width / DOOMGENERIC_RESX;
    uint32_t sy = fbi.height / DOOMGENERIC_RESY;
    scale = sx < sy ? sx : sy;
    if (scale < 1)
        scale = 1;
    uint32_t w = DOOMGENERIC_RESX * scale;
    uint32_t h = DOOMGENERIC_RESY * scale;
    off_x = fbi.width > w ? (fbi.width - w) / 2 : 0;
    off_y = fbi.height > h ? (fbi.height - h) / 2 : 0;
    row = malloc((size_t)fbi.width * sizeof(uint32_t));
    if (!row)
        die("malloc");
    memset(fb, 0, fbi.size);

    in_fd = open("/dev/input", O_RDONLY);
    if (in_fd < 0)
        die("/dev/input");
    ioctl(in_fd, ORION_INPUTIOC_FLUSH, 0);
    start_ms = now_ms();
    printf("doom: framebuffer %ux%u, escala %u\n", fbi.width, fbi.height, scale);
}

void DG_DrawFrame(void)
{
    uint32_t w = DOOMGENERIC_RESX * scale;
    if (w > fbi.width)
        w = fbi.width;
    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        const uint32_t *src = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
        for (uint32_t x = 0; x < w; x++)
            row[x] = pack(src[x / scale]);
        for (uint32_t k = 0; k < scale; k++) {
            uint32_t dy = off_y + y * scale + k;
            if (dy >= fbi.height)
                break;
            memcpy(fb + (size_t)dy * fbi.pitch + (size_t)off_x * 4, row, (size_t)w * 4);
        }
    }
    frames++;
    if (quit_after_ms && now_ms() - start_ms >= quit_after_ms) {
        printf("doom: %lu quadros em %llu ms\n", frames, (unsigned long long)(now_ms() - start_ms));
        orion_sound_report();
        exit(0);
    }
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)now_ms();
}

static const char plain_keys[0x36] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7',
    [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=', [0x10] = 'q', [0x11] = 'w',
    [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o',
    [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
};

static unsigned char translate(const struct orion_input_event *ev)
{
    int code = ev->a | ((ev->c & ORION_MOD_EXTENDED) ? 0x100 : 0);
    switch (code) {
    case 0x01: return KEY_ESCAPE;
    case 0x1C: case 0x11C: return KEY_ENTER;
    case 0x0F: return KEY_TAB;
    case 0x0E: return KEY_BACKSPACE;
    case 0x1D: case 0x11D: return KEY_FIRE;
    case 0x39: return KEY_USE;
    case 0x2A: case 0x36: return KEY_RSHIFT;
    case 0x38: case 0x138: return KEY_LALT;
    case 0x46: return KEY_PAUSE;
    case 0x48: case 0x148: return KEY_UPARROW;
    case 0x50: case 0x150: return KEY_DOWNARROW;
    case 0x4B: case 0x14B: return KEY_LEFTARROW;
    case 0x4D: case 0x14D: return KEY_RIGHTARROW;
    case 0x147: return KEY_HOME;
    case 0x14F: return KEY_END;
    case 0x149: return KEY_PGUP;
    case 0x151: return KEY_PGDN;
    case 0x152: return KEY_INS;
    case 0x153: return KEY_DEL;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    default:
        break;
    }
    if (code >= 0x3B && code <= 0x44)
        return (unsigned char)(0x80 + code);
    if (code > 0 && code < (int)sizeof(plain_keys) && plain_keys[code])
        return (unsigned char)plain_keys[code];
    return 0;
}

static void pump(void)
{
    struct pollfd p = { in_fd, POLLIN, 0 };
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        struct orion_input_event ev[16];
        ssize_t n = read(in_fd, ev, sizeof(ev));
        if (n <= 0)
            return;
        for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
            if (ev[i].type != ORION_EV_KEY_DOWN && ev[i].type != ORION_EV_KEY_UP)
                continue;
            unsigned char key = translate(&ev[i]);
            if (!key || kq_head - kq_tail >= KEY_QUEUE)
                continue;
            keyq[kq_head % KEY_QUEUE].key = key;
            keyq[kq_head % KEY_QUEUE].pressed = ev[i].type == ORION_EV_KEY_DOWN;
            kq_head++;
        }
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (kq_tail == kq_head)
        pump();
    if (kq_tail == kq_head)
        return 0;
    *pressed = keyq[kq_tail % KEY_QUEUE].pressed;
    *key = keyq[kq_tail % KEY_QUEUE].key;
    kq_tail++;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

static const char *const iwad_names[] = {
    "doom2.wad", "plutonia.wad", "tnt.wad", "doom.wad", "doom1.wad", "freedoom1.wad", "freedoom2.wad", "freedm.wad",
};

static char iwad_path[512];

static const char *find_iwad(void)
{
    const char *dirs[6];
    int n = 0;
    const char *env = getenv("DOOMWADDIR");
    if (env && *env)
        dirs[n++] = env;
    dirs[n++] = "/usr/share/games/doom";
    dirs[n++] = "/usr/local/share/games/doom";
    dirs[n++] = "/doom";
    dirs[n++] = ".";
    for (size_t w = 0; w < sizeof(iwad_names) / sizeof(iwad_names[0]); w++) {
        for (int d = 0; d < n; d++) {
            snprintf(iwad_path, sizeof(iwad_path), "%s/%s", dirs[d], iwad_names[w]);
            if (access(iwad_path, R_OK) == 0)
                return iwad_path;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static char *args[64];
    int out = 0;
    bool has_iwad = false;
    args[out++] = argv[0];
    for (int i = 1; i < argc && out < 60; i++) {
        if (strcmp(argv[i], "-orionquit") == 0 && i + 1 < argc) {
            quit_after_ms = (uint64_t)strtoul(argv[i + 1], NULL, 10) * 1000;
            i++;
            continue;
        }
        if (strcmp(argv[i], "-iwad") == 0) {
            has_iwad = true;
            if (i + 1 >= argc || access(argv[i + 1], R_OK) != 0) {
                fprintf(stderr, "doom: o IWAD %s nao existe ou nao pode ser lido\n", i + 1 < argc ? argv[i + 1] : "(faltou o arquivo)");
                return 1;
            }
        }
        args[out++] = argv[i];
    }
    if (!has_iwad) {
        const char *iwad = find_iwad();
        if (!iwad) {
            const char *env = getenv("DOOMWADDIR");
            fprintf(stderr,
                    "doom: nenhum IWAD encontrado (freedoom1.wad, freedoom2.wad, doom.wad, doom2.wad...) em %s%s"
                    "/usr/share/games/doom, /usr/local/share/games/doom, /doom e no diretorio atual.\n"
                    "doom: copie um .wad para /usr/share/games/doom, aponte DOOMWADDIR para a pasta dele ou use "
                    "doom -iwad /caminho/arquivo.wad\n",
                    env && *env ? env : "", env && *env ? " (DOOMWADDIR), " : "");
            return 1;
        }
        args[out++] = "-iwad";
        args[out++] = (char *)iwad;
    }
    args[out] = NULL;
    doomgeneric_Create(out, args);
    for (;;)
        doomgeneric_Tick();
}
