#pragma once

#include <stdbool.h>
#include <stdint.h>

struct framebuffer {
    void        *address;
    uint64_t    width;
    uint64_t    height;
    uint64_t    pitch;
    uint64_t    bpp;
    uint8_t     red_mask_size,    red_mask_shift;
    uint8_t     green_mask_size,  green_mask_shift;
    uint8_t     blue_mask_size,   blue_mask_shift;
};

#define COLOR_BLACK        0x000000
#define COLOR_RED          0xCC3333
#define COLOR_GREEN        0x33CC66
#define COLOR_YELLOW       0xE6C84C
#define COLOR_BLUE         0x3366CC
#define COLOR_MAGENTA      0xB050B0
#define COLOR_CYAN         0x33B8CC
#define COLOR_LIGHT_GRAY   0xC8C8C8
#define COLOR_DARK_GRAY    0x707070
#define COLOR_WHITE        0xF2F2F2
#define COLOR_BACKGROUND   0x0B0F1A

void console_init(const struct framebuffer *fb);
bool console_available(void);

void console_putc(char c);
void console_write(const char *s);
void console_clear(void);

void console_set_colors(uint32_t fg, uint32_t bg);
void console_get_colors(uint32_t *fg, uint32_t *bg);
void console_get_size(uint32_t *cols, uint32_t *rows);
void console_set_cursor_visible(bool visible);

const struct framebuffer *console_get_framebuffer(void);
uint32_t console_get_scale(void);