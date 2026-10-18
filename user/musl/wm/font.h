#pragma once

#include "gfx.h"

#define FONT_GLYPHS_CACHED 95

struct glyph {
    bool ready;
    int w, h, xoff, yoff, adv;
    uint8_t *alpha;
};

struct font {
    bool ok;
    void *impl;
    unsigned char *data;
    float scale;
    int ascent;
    int line_h;
    int advance;
    int cached;
    char name[48];
    int px;
    struct glyph glyphs[FONT_GLYPHS_CACHED];
};

bool font_load(struct font *f, const char *path, int px);
int font_text(struct gfx *g, struct font *f, int x, int y, const char *s, int n, uint32_t rgb);
int font_width(struct font *f, const char *s, int n);
int font_fit(struct font *f, const char *s, int max_w);
int font_alpha_levels(struct font *f, char c, int *w, int *h);
