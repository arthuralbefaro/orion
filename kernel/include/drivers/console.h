/*
 * console.h - console de texto sobre o framebuffer gráfico (linear, 32 bpp).
 *
 * Mantém uma grade de células (char + cores) em RAM e desenha os glifos no
 * framebuffer. Scroll = mover a grade e redesenhar (sem ler do framebuffer,
 * que em hardware real é lentíssimo para leitura).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Descrição do framebuffer, independente do bootloader. */
struct framebuffer {
    void    *address;   /* endereço virtual (já mapeado) */
    uint64_t width;     /* pixels */
    uint64_t height;    /* pixels */
    uint64_t pitch;     /* bytes por linha */
    uint16_t bpp;       /* bits por pixel (esperamos 32) */
    uint8_t  red_mask_size,   red_mask_shift;
    uint8_t  green_mask_size, green_mask_shift;
    uint8_t  blue_mask_size,  blue_mask_shift;
};

/* Cores em 0x00RRGGBB (paleta estilo VGA/terminal). */
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
#define COLOR_BACKGROUND   0x0B0F1A  /* azul-preto, fácil de ler */

void console_init(const struct framebuffer *fb);
bool console_available(void);

void console_putc(char c);
void console_write(const char *s);
void console_clear(void);
void console_redraw(void);
void console_set_graphics(bool on);
bool console_graphics(void);

void console_set_colors(uint32_t fg, uint32_t bg);
void console_get_colors(uint32_t *fg, uint32_t *bg);
void console_get_size(uint32_t *cols, uint32_t *rows);
void console_set_cursor_visible(bool visible);

/* Info do modo gráfico em uso (para o comando "fb" do shell etc.). */
const struct framebuffer *console_get_framebuffer(void);
uint32_t console_get_scale(void);
