#include "gfx.h"

#include <errno.h>
#include <fcntl.h>
#include "font8x8.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <orion.h>

_Static_assert(GFX_FONT_W == FONT_WIDTH && GFX_FONT_H == FONT_HEIGHT, "fonte 8x8");

#define CURSOR_W 12
#define CURSOR_H 19

static const char *const cursor_bits[CURSOR_H] = {
    "#           ",
    "##          ",
    "#*#         ",
    "#**#        ",
    "#***#       ",
    "#****#      ",
    "#*****#     ",
    "#******#    ",
    "#*******#   ",
    "#********#  ",
    "#*********# ",
    "#*****##### ",
    "#**#**#     ",
    "#*# #**#    ",
    "##   #**#   ",
    "#    #**#   ",
    "      #**#  ",
    "      #**#  ",
    "       ##   ",
};

struct rect rect_make(int x, int y, int w, int h)
{
    struct rect r = { x, y, w, h };
    return r;
}

bool rect_intersects(struct rect a, struct rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

struct rect rect_clip(struct rect a, struct rect b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool gfx_open(struct gfx *g, int fb_fd)
{
    memset(g, 0, sizeof(*g));
    if (ioctl(fb_fd, FBIOGET_INFO, &g->info) < 0) {
        fprintf(stderr, "wm: FBIOGET_INFO: %s\n", orion_strerror(errno));
        return false;
    }
    if (g->info.bpp != 32) {
        fprintf(stderr, "wm: so suporto 32 bpp (framebuffer tem %u)\n", g->info.bpp);
        return false;
    }

    g->width = (int)g->info.width;
    g->height = (int)g->info.height;
    g->clip = rect_make(0, 0, g->width, g->height);

    g->fb = mmap(NULL, g->info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (g->fb == MAP_FAILED) {
        fprintf(stderr, "wm: mmap do framebuffer: %s\n", orion_strerror(errno));
        return false;
    }

    size_t back_size = (size_t)g->width * (size_t)g->height * 4;
    g->back = mmap(NULL, back_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g->back == MAP_FAILED) {
        fprintf(stderr, "wm: mmap do backbuffer: %s\n", orion_strerror(errno));
        return false;
    }
    return true;
}

void gfx_close(struct gfx *g)
{
    if (g->back && g->back != MAP_FAILED)
        munmap(g->back, (size_t)g->width * (size_t)g->height * 4);
    if (g->fb && g->fb != MAP_FAILED)
        munmap(g->fb, g->info.size);
    g->back = NULL;
    g->fb = NULL;
}

uint32_t gfx_rgb(const struct gfx *g, uint32_t rgb)
{
    return fb_pack(&g->info, rgb);
}

void gfx_set_clip(struct gfx *g, struct rect r)
{
    g->clip = rect_clip(r, rect_make(0, 0, g->width, g->height));
}

void gfx_fill(struct gfx *g, struct rect r, uint32_t pixel)
{
    struct rect c = rect_clip(r, g->clip);
    for (int y = 0; y < c.h; y++) {
        uint32_t *row = g->back + (size_t)(c.y + y) * g->width + c.x;
        for (int x = 0; x < c.w; x++)
            row[x] = pixel;
    }
}

static void draw_glyph(struct gfx *g, int x, int y, unsigned char ch, uint32_t pixel, int zoom)
{
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR)
        ch = '?';
    const uint8_t *glyph = font8x8_basic[ch - FONT_FIRST_CHAR];
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        if (!bits)
            continue;
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            if (!((bits >> gx) & 1))
                continue;
            gfx_fill(g, rect_make(x + gx * zoom, y + gy * zoom, zoom, zoom), pixel);
        }
    }
}

void gfx_text_n(struct gfx *g, int x, int y, const char *s, int n, uint32_t pixel, int zoom)
{
    int cell = FONT_WIDTH * zoom;
    for (int i = 0; i < n && s[i]; i++) {
        int cx = x + i * cell;
        if (cx + cell <= g->clip.x || cx >= g->clip.x + g->clip.w)
            continue;
        if (y + FONT_HEIGHT * zoom <= g->clip.y || y >= g->clip.y + g->clip.h)
            return;
        if (s[i] != ' ')
            draw_glyph(g, cx, y, (unsigned char)s[i], pixel, zoom);
    }
}

void gfx_text(struct gfx *g, int x, int y, const char *s, uint32_t pixel, int zoom)
{
    gfx_text_n(g, x, y, s, (int)strlen(s), pixel, zoom);
}

void gfx_char(struct gfx *g, int x, int y, char c, uint32_t pixel, int zoom)
{
    if (c == ' ' || c == 0)
        return;
    if (x + GFX_FONT_W * zoom <= g->clip.x || x >= g->clip.x + g->clip.w)
        return;
    if (y + GFX_FONT_H * zoom <= g->clip.y || y >= g->clip.y + g->clip.h)
        return;
    draw_glyph(g, x, y, (unsigned char)c, pixel, zoom);
}

struct rect gfx_cursor_rect(const struct gfx *g)
{
    return rect_make(g->cursor_x, g->cursor_y, CURSOR_W, CURSOR_H);
}

void gfx_cursor_move(struct gfx *g, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g->width - 2) x = g->width - 2;
    if (y > g->height - 2) y = g->height - 2;
    g->cursor_x = x;
    g->cursor_y = y;
}

static void paint_cursor(struct gfx *g, struct rect area)
{
    uint32_t edge = gfx_rgb(g, 0x101014);
    uint32_t body = gfx_rgb(g, 0xFFFFFF);
    for (int y = 0; y < CURSOR_H; y++) {
        int py = g->cursor_y + y;
        if (py < area.y || py >= area.y + area.h || py >= g->height)
            continue;
        const char *row = cursor_bits[y];
        uint32_t *dst = (uint32_t *)(g->fb + (size_t)py * g->info.pitch);
        for (int x = 0; x < CURSOR_W; x++) {
            int px = g->cursor_x + x;
            if (px < area.x || px >= area.x + area.w || px >= g->width)
                continue;
            if (row[x] == '#')
                dst[px] = edge;
            else if (row[x] == '*')
                dst[px] = body;
        }
    }
}

void gfx_damage(struct gfx *g, struct rect r)
{
    r = rect_clip(r, rect_make(0, 0, g->width, g->height));
    if (r.w <= 0 || r.h <= 0)
        return;

    for (int i = 0; i < g->dirty_count; i++) {
        if (rect_intersects(g->dirty[i], r)) {
            g->dirty[i] = rect_union(g->dirty[i], r);
            return;
        }
    }
    if (g->dirty_count == GFX_MAX_DIRTY) {
        struct rect all = g->dirty[0];
        for (int i = 1; i < g->dirty_count; i++)
            all = rect_union(all, g->dirty[i]);
        g->dirty[0] = rect_union(all, r);
        g->dirty_count = 1;
        return;
    }
    g->dirty[g->dirty_count++] = r;
}

bool gfx_has_damage(const struct gfx *g)
{
    return g->dirty_count > 0;
}

void gfx_flush(struct gfx *g, void (*compose)(struct rect r))
{
    if (!g->dirty_count)
        return;
    for (int i = 0; i < g->dirty_count; i++) {
        struct rect r = g->dirty[i];
        gfx_set_clip(g, r);
        compose(r);
        for (int y = 0; y < r.h; y++) {
            const uint32_t *src = g->back + (size_t)(r.y + y) * g->width + r.x;
            uint32_t *dst = (uint32_t *)(g->fb + (size_t)(r.y + y) * g->info.pitch) + r.x;
            memcpy(dst, src, (size_t)r.w * 4);
        }
        paint_cursor(g, r);
        g->flushed_pixels += (unsigned long)r.w * (unsigned long)r.h;
    }
    gfx_set_clip(g, rect_make(0, 0, g->width, g->height));
    g->dirty_count = 0;
    g->frames++;
}
