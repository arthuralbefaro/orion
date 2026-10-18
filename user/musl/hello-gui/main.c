#include "font8x8.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WM_SOCKET "/run/wmapp.sock"
#define WIDTH     360
#define HEIGHT    200
#define ZOOM      2
#define MARKS     16

static uint32_t *pixels;
static int sock = -1;
static char in[512];
static int inlen;
static int keys, clicks;
static char last_key = ' ';
static int mark_x[MARKS], mark_y[MARKS], marks;

static void send_line(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0)
        write(sock, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

static int read_line(char *out, size_t size)
{
    for (;;) {
        char *nl = memchr(in, '\n', (size_t)inlen);
        if (nl) {
            size_t len = (size_t)(nl - in);
            if (len >= size)
                len = size - 1;
            memcpy(out, in, len);
            out[len] = 0;
            int used = (int)(nl - in) + 1;
            memmove(in, in + used, (size_t)(inlen - used));
            inlen -= used;
            return 1;
        }
        if (inlen >= (int)sizeof(in) - 1)
            return -1;
        ssize_t n = read(sock, in + inlen, sizeof(in) - 1 - (size_t)inlen);
        if (n <= 0)
            return 0;
        inlen += (int)n;
    }
}

static void fill(int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < WIDTH && j < HEIGHT)
                pixels[j * WIDTH + i] = rgb;
}

static void text(int x, int y, const char *s, uint32_t rgb)
{
    for (; *s; s++, x += FONT_WIDTH * ZOOM) {
        unsigned char c = (unsigned char)*s;
        if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
            c = '?';
        const uint8_t *glyph = font8x8_basic[c - FONT_FIRST_CHAR];
        for (int gy = 0; gy < FONT_HEIGHT; gy++)
            for (int gx = 0; gx < FONT_WIDTH; gx++)
                if ((glyph[gy] >> gx) & 1)
                    fill(x + gx * ZOOM, y + gy * ZOOM, ZOOM, ZOOM, rgb);
    }
}

static void draw(void)
{
    for (int y = 0; y < HEIGHT; y++) {
        uint32_t r = (uint32_t)(40 + y * 120 / HEIGHT);
        uint32_t b = (uint32_t)(150 + y * 80 / HEIGHT);
        for (int x = 0; x < WIDTH; x++)
            pixels[y * WIDTH + x] = (r << 16) | ((uint32_t)(x * 90 / WIDTH) << 8) | b;
    }
    char line[64];
    text(12, 12, "hello-gui", 0xFFE070);
    snprintf(line, sizeof(line), "processo %d", (int)getpid());
    text(12, 40, line, 0xFFFFFF);
    snprintf(line, sizeof(line), "teclas: %d (%c)", keys, last_key);
    text(12, 64, line, 0xFFFFFF);
    snprintf(line, sizeof(line), "cliques: %d", clicks);
    text(12, 88, line, 0xFFFFFF);
    text(12, 170, "q fecha", 0xC0C8FF);
    for (int i = 0; i < marks; i++)
        fill(mark_x[i] - 4, mark_y[i] - 4, 9, 9, 0x40FF80);
    send_line("mostra\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WM_SOCKET);
    if (sock < 0 || connect(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "hello-gui: %s: %s (o wm esta rodando?)\n", WM_SOCKET, strerror(errno));
        return 1;
    }
    send_line("janela %d %d Ola do hello-gui\n", WIDTH, HEIGHT);
    char line[256], path[128] = "";
    int id = -1, w = 0, h = 0;
    if (read_line(line, sizeof(line)) <= 0 || sscanf(line, "ok %d %127s %d %d", &id, path, &w, &h) != 4) {
        fprintf(stderr, "hello-gui: o wm recusou: %s\n", line);
        return 1;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    pixels = fd >= 0 ? mmap(NULL, (size_t)WIDTH * HEIGHT * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                     : MAP_FAILED;
    if (fd >= 0)
        close(fd);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "hello-gui: %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("hello-gui: janela %d aberta (%dx%d), desenhando em %s\n", id, w, h, path);
    draw();
    for (;;) {
        int r = read_line(line, sizeof(line));
        if (r <= 0) {
            printf("hello-gui: o wm fechou a conexao\n");
            return 0;
        }
        int ascii, code, mods, x, y;
        unsigned b;
        if (sscanf(line, "tecla %d %d %d", &ascii, &code, &mods) == 3) {
            if (ascii == 'q') {
                printf("hello-gui: q, fechando\n");
                send_line("fecha\n");
                return 0;
            }
            if (ascii < 32 || ascii > 126)
                continue;
            keys++;
            last_key = (char)ascii;
            send_line("titulo hello-gui: tecla %c\n", last_key);
            draw();
        } else if (sscanf(line, "clique %d %d %u", &x, &y, &b) == 3) {
            clicks++;
            if (marks < MARKS) {
                mark_x[marks] = x;
                mark_y[marks] = y;
                marks++;
            }
            send_line("titulo hello-gui: clique %d,%d\n", x, y);
            draw();
        } else if (strcmp(line, "fechar") == 0) {
            printf("hello-gui: o wm fechou a janela\n");
            return 0;
        }
    }
}
