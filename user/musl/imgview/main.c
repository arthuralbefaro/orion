#include "gui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#pragma clang diagnostic pop

#define MAX_FILES 64
#define VIEW_W    640
#define VIEW_H    380

static struct gui g;
static struct gui_widget *view, *info;
static char dir[256];
static char *files[MAX_FILES];
static int nfiles, current;
static uint32_t *pixels;
static int img_w, img_h;
static bool fit = true;

static bool is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash)
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path);
    else
        snprintf(dir, sizeof(dir), ".");
    if (!dir[0])
        snprintf(dir, sizeof(dir), "/");
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nfiles < MAX_FILES)
        if (is_image(e->d_name))
            files[nfiles++] = strdup(e->d_name);
    if (d)
        closedir(d);
    qsort(files, (size_t)nfiles, sizeof(files[0]), by_name);
    const char *base = slash ? slash + 1 : path;
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i], base))
            current = i;
}

static void load_current(void)
{
    free(pixels);
    pixels = NULL;
    img_w = img_h = 0;
    if (!nfiles) {
        gui_set_text(info, "nenhuma imagem em %s", dir);
        gui_set_title(&g, "imgview: sem imagens");
        view->version++;
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", files[current]);
    int n;
    unsigned char *px = stbi_load(path, &img_w, &img_h, &n, 3);
    if (px) {
        pixels = malloc((size_t)img_w * (size_t)img_h * 4);
        for (size_t i = 0; pixels && i < (size_t)img_w * (size_t)img_h; i++)
            pixels[i] = ((uint32_t)px[i * 3] << 16) | ((uint32_t)px[i * 3 + 1] << 8) | px[i * 3 + 2];
        stbi_image_free(px);
    }
    if (pixels) {
        gui_set_text(info, "%d/%d  %s  %dx%d  %s", current + 1, nfiles, files[current], img_w, img_h,
                     fit ? "ajustada" : "1:1");
        gui_set_title(&g, "imgview: %s %dx%d", files[current], img_w, img_h);
    } else {
        gui_set_text(info, "%s: nao decodificou", files[current]);
        gui_set_title(&g, "imgview: erro em %s", files[current]);
    }
    view->version++;
}

static void paint(struct gui *gu, struct gui_widget *w)
{
    if (!pixels)
        return;
    int dw = img_w, dh = img_h;
    if (fit) {
        dw = w->w;
        dh = (int)((long)img_h * w->w / img_w);
        if (dh > w->h) {
            dh = w->h;
            dw = (int)((long)img_w * w->h / img_h);
        }
    }
    int ox = w->x + (w->w - dw) / 2, oy = w->y + (w->h - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int ty = oy + y;
        if (ty < w->y || ty >= w->y + w->h)
            continue;
        int sy = (int)((long)y * img_h / dh);
        for (int x = 0; x < dw; x++) {
            int tx = ox + x;
            if (tx < w->x || tx >= w->x + w->w)
                continue;
            gu->pixels[(size_t)ty * (size_t)gu->width + (size_t)tx] =
                pixels[(size_t)sy * (size_t)img_w + (size_t)((long)x * img_w / dw)];
        }
    }
}

static void step(int d)
{
    if (!nfiles)
        return;
    current = (current + d + nfiles) % nfiles;
    load_current();
}

static void on_prev(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    step(-1);
}

static void on_next(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    step(1);
}

static void on_fit(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    fit = !fit;
    load_current();
}

static void on_close(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    gui_quit(gu);
}

static bool on_key(struct gui *gu, int ascii, int scancode, int mods)
{
    bool ext = (mods & GUI_MOD_EXTENDED) != 0;
    if (!ext && ascii == 'q') {
        gui_quit(gu);
        return true;
    }
    if (!ext && ascii == 'f') {
        on_fit(gu, NULL);
        return true;
    }
    if (ext && (scancode == GUI_SC_RIGHT || scancode == GUI_SC_LEFT)) {
        step(scancode == GUI_SC_RIGHT ? 1 : -1);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    const char *start = argc > 1 ? argv[1] : "/usr/share/wallpapers/orion.png";
    if (!gui_open(&g, VIEW_W + 20, VIEW_H + 110, "imgview"))
        return 1;
    g.on_key = on_key;
    view = gui_canvas(&g, 10, 10, VIEW_W, VIEW_H, paint, NULL);
    info = gui_label(&g, 10, VIEW_H + 20, VIEW_W, "");
    gui_button(&g, 10, VIEW_H + 54, 150, "Anterior", on_prev);
    gui_button(&g, 175, VIEW_H + 54, 150, "Proxima", on_next);
    gui_button(&g, 340, VIEW_H + 54, 150, "Ajustar", on_fit);
    gui_button(&g, 505, VIEW_H + 54, 150, "Fechar", on_close);
    scan(start);
    load_current();
    gui_run(&g);
    return 0;
}
