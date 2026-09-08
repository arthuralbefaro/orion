#pragma once

#include <sys/types.h>

#define FBIOGET_INFO 0x4600
#define FBIOREDRAW   0x4601

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t red_mask_size,   red_mask_shift;
    uint32_t green_mask_size, green_mask_shift;
    uint32_t blue_mask_size,  blue_mask_shift;
    uint64_t size;
};

static inline uint32_t fb_pack(const struct fb_info *fb, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return ((r >> (8 - fb->red_mask_size))   << fb->red_mask_shift) |
           ((g >> (8 - fb->green_mask_size)) << fb->green_mask_shift) |
           ((b >> (8 - fb->blue_mask_size))  << fb->blue_mask_shift);
}
