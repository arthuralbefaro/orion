#pragma once

#include <stdbool.h>
#include <stdint.h>

struct image {
    int w, h;
    uint32_t *rgb;
    const char *format;
};

bool image_load(const char *path, struct image *out);
bool image_scale(const struct image *in, int w, int h, struct image *out);
void image_free(struct image *img);
