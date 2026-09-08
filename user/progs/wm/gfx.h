#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/fb.h>

#define GFX_FONT_W   8
#define GFX_FONT_H   8
#define GFX_ZOOM     2
#define GFX_CELL_W   (GFX_FONT_W * GFX_ZOOM)
#define GFX_CELL_H   ((GFX_FONT_H + 1) * GFX_ZOOM)
#define GFX_MAX_DIRTY 24

struct rect {
    int x, y, w, h;
};

struct gfx {
    struct fb_info info;
    uint32_t *back;
    uint8_t *fb;
    int width, height;
    struct rect clip;
    int cursor_x, cursor_y;
    struct rect dirty[GFX_MAX_DIRTY];
    int dirty_count;
    unsigned long flushed_pixels;
    unsigned long frames;
};

struct rect rect_make(int x, int y, int w, int h);
bool rect_intersects(struct rect a, struct rect b);
struct rect rect_clip(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
bool rect_contains(struct rect r, int x, int y);

bool gfx_open(struct gfx *g, int fb_fd);
void gfx_close(struct gfx *g);

uint32_t gfx_rgb(const struct gfx *g, uint32_t rgb);
void gfx_set_clip(struct gfx *g, struct rect r);
void gfx_fill(struct gfx *g, struct rect r, uint32_t pixel);
void gfx_text(struct gfx *g, int x, int y, const char *s, uint32_t pixel, int zoom);
void gfx_char(struct gfx *g, int x, int y, char c, uint32_t pixel, int zoom);
void gfx_text_n(struct gfx *g, int x, int y, const char *s, int n, uint32_t pixel, int zoom);

void gfx_cursor_move(struct gfx *g, int x, int y);
struct rect gfx_cursor_rect(const struct gfx *g);

void gfx_damage(struct gfx *g, struct rect r);
bool gfx_has_damage(const struct gfx *g);
void gfx_flush(struct gfx *g, void (*compose)(struct rect r));
