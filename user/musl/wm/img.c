#include "img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *format_of(const char *path)
{
    unsigned char m[3] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f)
        return "?";
    size_t n = fread(m, 1, sizeof(m), f);
    fclose(f);
    if (n == 3 && m[0] == 0x89 && m[1] == 'P' && m[2] == 'N')
        return "PNG";
    if (n == 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF)
        return "JPEG";
    return "?";
}

bool image_load(const char *path, struct image *out)
{
    memset(out, 0, sizeof(*out));
    int w, h, n;
    unsigned char *px = stbi_load(path, &w, &h, &n, 3);
    if (!px)
        return false;
    out->rgb = malloc((size_t)w * (size_t)h * 4);
    if (!out->rgb) {
        stbi_image_free(px);
        return false;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        out->rgb[i] = ((uint32_t)px[i * 3] << 16) | ((uint32_t)px[i * 3 + 1] << 8) | px[i * 3 + 2];
    stbi_image_free(px);
    out->w = w;
    out->h = h;
    out->format = format_of(path);
    return true;
}

bool image_scale(const struct image *in, int w, int h, struct image *out)
{
    memset(out, 0, sizeof(*out));
    out->rgb = malloc((size_t)w * (size_t)h * 4);
    if (!out->rgb)
        return false;
    for (int y = 0; y < h; y++) {
        int sy = (int)((long)y * in->h / h);
        for (int x = 0; x < w; x++)
            out->rgb[(size_t)y * (size_t)w + (size_t)x] = in->rgb[(size_t)sy * (size_t)in->w + (size_t)((long)x * in->w / w)];
    }
    out->w = w;
    out->h = h;
    out->format = in->format;
    return true;
}

void image_free(struct image *img)
{
    free(img->rgb);
    img->rgb = NULL;
}
