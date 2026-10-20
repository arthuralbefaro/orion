#pragma once

#include <kernel/compiler.h>

#define FBIOGET_INFO 0x4600
#define FBIOREDRAW   0x4601
#define FBIOSET_MODE 0x4602

struct fb_mode {
    uint32_t width;
    uint32_t height;
};

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

struct file_ops;

bool fbdev_available(void);
bool fbdev_get_info(struct fb_info *out);
uint64_t fbdev_size(void);
uint64_t fbdev_mapped_pages(void);
const struct file_ops *fbdev_ops(void);
struct file *fbdev_open(void);
uint32_t fbdev_graphics_users(void);
