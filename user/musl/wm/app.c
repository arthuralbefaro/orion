#include "app.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define APP_LINE  512
#define APP_MIN_W 64
#define APP_MIN_H 32
#define APP_MAX_W 800
#define APP_MAX_H 520

struct app {
    bool used;
    int fd;
    struct window *win;
    char in[APP_LINE];
    int inlen;
    uint32_t *pixels;
    size_t len;
    char path[64];
    unsigned long frames;
};

static struct app apps[APP_MAX];
static int listen_fd = -1;
static struct desktop *desk;
static struct gfx *gfx;
static int buffers;
static int placed;

void app_init(struct desktop *d, struct gfx *g)
{
    desk = d;
    gfx = g;
    mkdir(APP_DIR, 0755);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "wm: socket de apps: %s\n", orion_strerror(errno));
        return;
    }
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", APP_SOCKET);
    unlink(APP_SOCKET);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(listen_fd, APP_MAX) < 0) {
        fprintf(stderr, "wm: %s: %s\n", APP_SOCKET, orion_strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    fprintf(stderr, "wm: apps em %s\n", APP_SOCKET);
}

static void send_line(struct app *a, const char *fmt, ...)
{
    char line[APP_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(line))
        n = (int)sizeof(line) - 1;
    const char *p = line;
    while (n > 0) {
        ssize_t w = write(a->fd, p, (size_t)n);
        if (w <= 0)
            return;
        p += w;
        n -= (int)w;
    }
}

static void release_buffer(struct app *a)
{
    if (a->pixels) {
        munmap(a->pixels, a->len);
        a->pixels = NULL;
    }
    if (a->path[0]) {
        unlink(a->path);
        a->path[0] = 0;
    }
}

static void drop(struct app *a, const char *why)
{
    if (a->win) {
        fprintf(stderr, "wm: app da janela %d saiu (%s) depois de %lu quadro(s)\n", a->win->id, why, a->frames);
        desk_damage_window(desk, a->win);
        desk_remove(desk, a->win);
        struct window *top = desk_focused(desk);
        if (top)
            desk_damage_window(desk, top);
        a->win = NULL;
    }
    release_buffer(a);
    close(a->fd);
    a->used = false;
}

static struct app *app_of(struct window *w)
{
    for (int i = 0; i < APP_MAX; i++)
        if (apps[i].used && apps[i].win == w)
            return &apps[i];
    return NULL;
}

static void open_window(struct app *a, int w, int h, const char *title)
{
    if (a->win) {
        send_line(a, "erro: ja tem janela\n");
        return;
    }
    if (w < APP_MIN_W || h < APP_MIN_H || w > APP_MAX_W || h > APP_MAX_H) {
        send_line(a, "erro: tamanho entre %dx%d e %dx%d\n", APP_MIN_W, APP_MIN_H, APP_MAX_W, APP_MAX_H);
        return;
    }
    snprintf(a->path, sizeof(a->path), "%s/app-%d.buf", APP_DIR, ++buffers);
    a->len = (size_t)w * (size_t)h * 4;
    int fd = open(a->path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0 || ftruncate(fd, (off_t)a->len) < 0) {
        send_line(a, "erro: buffer: %s\n", orion_strerror(errno));
        if (fd >= 0)
            close(fd);
        unlink(a->path);
        a->path[0] = 0;
        return;
    }
    void *m = mmap(NULL, a->len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        send_line(a, "erro: mmap: %s\n", orion_strerror(errno));
        release_buffer(a);
        return;
    }
    a->pixels = m;
    int step = placed++ % 5;
    struct rect f = rect_make(220 + step * 30, 90 + step * 26, w + WIN_CHROME_X, h + WIN_CHROME_Y);
    struct window *win = desk_new(desk, WIN_APP, title[0] ? title : "app", f);
    if (!win) {
        send_line(a, "erro: janelas demais\n");
        release_buffer(a);
        return;
    }
    win->pixels = a->pixels;
    win->pix_w = w;
    win->pix_h = h;
    a->win = win;
    desk_damage_window(desk, win);
    fprintf(stderr, "wm: app abriu a janela %d (%dx%d, '%s') com o buffer em %s\n", win->id, w, h, win->title,
            a->path);
    send_line(a, "ok %d %s %d %d\n", win->id, a->path, w, h);
}

static void command(struct app *a, char *line)
{
    if (strncmp(line, "janela ", 7) == 0) {
        int w = 0, h = 0, used = 0;
        if (sscanf(line + 7, "%d %d %n", &w, &h, &used) < 2) {
            send_line(a, "erro: uso: janela <largura> <altura> <titulo>\n");
            return;
        }
        open_window(a, w, h, line + 7 + used);
    } else if (strcmp(line, "mostra") == 0) {
        if (!a->win)
            return;
        if (a->frames++ == 0)
            fprintf(stderr, "wm: janela %d mostrou o primeiro quadro\n", a->win->id);
        gfx_damage(gfx, win_content(a->win));
    } else if (strncmp(line, "titulo ", 7) == 0) {
        if (!a->win)
            return;
        snprintf(a->win->title, sizeof(a->win->title), "%s", line + 7);
        fprintf(stderr, "wm: janela %d agora se chama '%s'\n", a->win->id, a->win->title);
        gfx_damage(gfx, win_titlebar(a->win));
    } else if (strcmp(line, "fecha") == 0) {
        drop(a, "pediu para fechar");
    } else {
        send_line(a, "erro: comandos: janela, mostra, titulo, fecha\n");
    }
}

static void serve(struct app *a)
{
    ssize_t n = read(a->fd, a->in + a->inlen, sizeof(a->in) - 1 - (size_t)a->inlen);
    if (n < 0 && errno == EAGAIN)
        return;
    if (n <= 0) {
        drop(a, "desconectou");
        return;
    }
    a->inlen += (int)n;
    a->in[a->inlen] = 0;
    char *start = a->in;
    char *nl;
    while (a->used && (nl = strchr(start, '\n'))) {
        *nl = 0;
        if (nl > start && nl[-1] == '\r')
            nl[-1] = 0;
        command(a, start);
        start = nl + 1;
    }
    if (!a->used)
        return;
    a->inlen = (int)strlen(start);
    memmove(a->in, start, (size_t)a->inlen + 1);
    if (a->inlen >= (int)sizeof(a->in) - 1)
        drop(a, "linha longa demais");
}

static void accept_apps(void)
{
    for (;;) {
        int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0)
            return;
        struct app *a = NULL;
        for (int i = 0; i < APP_MAX && !a; i++)
            if (!apps[i].used)
                a = &apps[i];
        if (!a) {
            close(c);
            continue;
        }
        memset(a, 0, sizeof(*a));
        a->used = true;
        a->fd = c;
    }
}

int app_pollfds(struct pollfd *fds, int max)
{
    int n = 0;
    if (listen_fd >= 0 && n < max) {
        fds[n].fd = listen_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    for (int i = 0; i < APP_MAX && n < max; i++) {
        if (!apps[i].used)
            continue;
        fds[n].fd = apps[i].fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    return n;
}

void app_service(const struct pollfd *fds, int n)
{
    for (int k = 0; k < n; k++) {
        if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        if (fds[k].fd == listen_fd) {
            accept_apps();
            continue;
        }
        for (int i = 0; i < APP_MAX; i++)
            if (apps[i].used && apps[i].fd == fds[k].fd)
                serve(&apps[i]);
    }
}

void app_key(struct window *w, int32_t ascii, int32_t scancode, int32_t mods)
{
    struct app *a = app_of(w);
    if (a)
        send_line(a, "tecla %d %d %d\n", (int)ascii, (int)scancode, (int)mods);
}

void app_click(struct window *w, int x, int y, uint32_t buttons)
{
    struct app *a = app_of(w);
    if (a)
        send_line(a, "clique %d %d %u\n", x, y, buttons);
}

void app_close_window(struct window *w)
{
    struct app *a = app_of(w);
    if (!a)
        return;
    send_line(a, "fechar\n");
    drop(a, "janela fechada no wm");
}

void app_shutdown(void)
{
    for (int i = 0; i < APP_MAX; i++)
        if (apps[i].used) {
            send_line(&apps[i], "fechar\n");
            drop(&apps[i], "wm encerrando");
        }
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(APP_SOCKET);
        listen_fd = -1;
    }
}
