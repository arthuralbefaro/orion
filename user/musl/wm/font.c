#include "font.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#pragma clang diagnostic pop

static unsigned char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) {
            free(buf);
            buf = NULL;
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    *len = got;
    return buf;
}

bool font_load(struct font *f, const char *path, int px)
{
    memset(f, 0, sizeof(*f));
    size_t len = 0;
    f->data = read_file(path, &len);
    if (!f->data)
        return false;
    stbtt_fontinfo *info = calloc(1, sizeof(*info));
    if (!info || !stbtt_InitFont(info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        free(info);
        free(f->data);
        f->data = NULL;
        return false;
    }
    f->impl = info;
    f->px = px;
    f->scale = stbtt_ScaleForPixelHeight(info, (float)px);
    int ascent, descent, gap, adv, lsb;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    f->ascent = (int)(ascent * f->scale + 0.5f);
    f->line_h = (int)((ascent - descent + gap) * f->scale + 0.5f);
    stbtt_GetCodepointHMetrics(info, 'M', &adv, &lsb);
    f->advance = (int)(adv * f->scale + 0.5f);
    const char *base = strrchr(path, '/');
    snprintf(f->name, sizeof(f->name), "%s", base ? base + 1 : path);
    f->ok = true;
    return true;
}

static struct glyph *glyph_of(struct font *f, unsigned char c)
{
    if (c < 32 || c > 126)
        c = '?';
    struct glyph *gl = &f->glyphs[c - 32];
    if (gl->ready)
        return gl;
    stbtt_fontinfo *info = f->impl;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(info, c, &adv, &lsb);
    gl->adv = (int)(adv * f->scale + 0.5f);
    gl->alpha = stbtt_GetCodepointBitmap(info, 0, f->scale, c, &gl->w, &gl->h, &gl->xoff, &gl->yoff);
    gl->ready = true;
    f->cached++;
    return gl;
}

int font_text(struct gfx *g, struct font *f, int x, int y, const char *s, int n, uint32_t rgb)
{
    int start = x;
    int base = y + f->ascent;
    for (int i = 0; i < n && s[i]; i++) {
        struct glyph *gl = glyph_of(f, (unsigned char)s[i]);
        if (gl->alpha)
            gfx_blend_alpha(g, x + gl->xoff, base + gl->yoff, gl->alpha, gl->w, gl->h, rgb);
        x += gl->adv;
    }
    return x - start;
}

int font_width(struct font *f, const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n && s[i]; i++)
        w += glyph_of(f, (unsigned char)s[i])->adv;
    return w;
}

int font_alpha_levels(struct font *f, char c, int *w, int *h)
{
    struct glyph *gl = glyph_of(f, (unsigned char)c);
    bool seen[256] = { false };
    int levels = 0;
    for (int i = 0; gl->alpha && i < gl->w * gl->h; i++)
        if (gl->alpha[i] && !seen[gl->alpha[i]]) {
            seen[gl->alpha[i]] = true;
            levels++;
        }
    *w = gl->w;
    *h = gl->h;
    return levels;
}

int font_fit(struct font *f, const char *s, int max_w)
{
    int w = 0, i = 0;
    for (; s[i]; i++) {
        int a = glyph_of(f, (unsigned char)s[i])->adv;
        if (w + a > max_w)
            break;
        w += a;
    }
    return i;
}
