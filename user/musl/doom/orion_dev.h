#pragma once

#include <stdint.h>

#define ORION_FBIOGET_INFO        0x4600
#define ORION_INPUTIOC_SETTIMEOUT 0x4900
#define ORION_INPUTIOC_FLUSH      0x4901

#define ORION_EV_KEY_DOWN 1
#define ORION_EV_KEY_UP   2

#define ORION_MOD_EXTENDED (1 << 4)

struct orion_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t red_mask_size, red_mask_shift;
    uint32_t green_mask_size, green_mask_shift;
    uint32_t blue_mask_size, blue_mask_shift;
    uint64_t size;
};

struct orion_input_event {
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
};
