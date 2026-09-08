/*
 * console.c - console de texto sobre framebuffer.
 */
#include <drivers/console.h>
#include <drivers/font8x8.h>
#include <lib/string.h>
#include <kernel/compiler.h>

#define MAX_COLS 240
#define MAX_ROWS 135
#define TAB_WIDTH 4
#define LINE_GAP  1   /* linhas de pixel (x zoom) entre uma linha de texto e outra */

struct cell {
    char     ch;
    uint32_t fg;
    uint32_t bg;
};

static struct framebuffer fb;
static bool     initialized = false;
static uint32_t scale = 1;          /* fator de zoom da fonte */
static uint32_t cell_w, cell_h;     /* tamanho da célula em pixels */
static uint32_t cols, rows;         /* tamanho da grade */
static uint32_t cur_col, cur_row;   /* cursor */
static uint32_t cur_fg = COLOR_LIGHT_GRAY;
static uint32_t cur_bg = COLOR_BACKGROUND;
static bool     cursor_visible = true;

static struct cell grid[MAX_ROWS][MAX_COLS];

/* ---------- Pixels ---------- */

/* Converte 0x00RRGGBB para o formato de pixel do framebuffer. */
static inline uint32_t pack_color(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return ((r >> (8 - fb.red_mask_size))   << fb.red_mask_shift) |
           ((g >> (8 - fb.green_mask_size)) << fb.green_mask_shift) |
           ((b >> (8 - fb.blue_mask_size))  << fb.blue_mask_shift);
}

static inline void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixel)
{
    uint8_t *base = (uint8_t *)fb.address + (uint64_t)y * fb.pitch + (uint64_t)x * 4;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(base + (uint64_t)j * fb.pitch);
        for (uint32_t i = 0; i < w; i++)
            row[i] = pixel;
    }
}

/* Desenha uma célula da grade (opcionalmente em vídeo inverso, p/ cursor). */
static void draw_cell(uint32_t col, uint32_t row, bool inverse)
{
    const struct cell *c = &grid[row][col];
    uint32_t fg = pack_color(inverse ? c->bg : c->fg);
    uint32_t bg = pack_color(inverse ? c->fg : c->bg);

    uint8_t ch = (uint8_t)c->ch;
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR)
        ch = '?';
    const uint8_t *glyph = font8x8_basic[ch - FONT_FIRST_CHAR];

    uint32_t px = col * cell_w;
    uint32_t py = row * cell_h;

    /* Espaço entre linhas: só fundo. */
    fill_rect(px, py + FONT_HEIGHT * scale, cell_w, LINE_GAP * scale, bg);

    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t pixel = (bits >> gx) & 1 ? fg : bg;
            if (scale == 1) {
                uint32_t *p = (uint32_t *)((uint8_t *)fb.address +
                                           (uint64_t)(py + gy) * fb.pitch +
                                           (uint64_t)(px + gx) * 4);
                *p = pixel;
            } else {
                fill_rect(px + gx * scale, py + gy * scale, scale, scale, pixel);
            }
        }
    }
}

static void redraw_all(void)
{
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++)
            draw_cell(c, r, false);
}

static void draw_cursor(bool on)
{
    if (!cursor_visible)
        return;
    if (cur_col < cols && cur_row < rows)
        draw_cell(cur_col, cur_row, on);
}

/* ---------- Grade ---------- */

static void clear_row(uint32_t row)
{
    for (uint32_t c = 0; c < cols; c++) {
        grid[row][c].ch = ' ';
        grid[row][c].fg = cur_fg;
        grid[row][c].bg = cur_bg;
    }
}

static void scroll_up(void)
{
    memmove(&grid[0], &grid[1], sizeof(grid[0]) * (rows - 1));
    clear_row(rows - 1);
    redraw_all();
}

static void newline(void)
{
    cur_col = 0;
    cur_row++;
    if (cur_row >= rows) {
        cur_row = rows - 1;
        scroll_up();
    }
}

static void put_glyph(char ch)
{
    if (cur_col >= cols)
        newline();
    struct cell *c = &grid[cur_row][cur_col];
    c->ch = ch;
    c->fg = cur_fg;
    c->bg = cur_bg;
    draw_cell(cur_col, cur_row, false);
    cur_col++;
}

/* ---------- API ---------- */

void console_init(const struct framebuffer *info)
{
    if (!info || !info->address || info->bpp != 32)
        return;

    fb = *info;

    /* Escolhe o zoom da fonte pela largura da tela. */
    if (fb.width >= 2560)
        scale = 3;
    else if (fb.width >= 1280)
        scale = 2;
    else
        scale = 1;

    cell_w = FONT_WIDTH * scale;
    cell_h = (FONT_HEIGHT + LINE_GAP) * scale;
    cols = (uint32_t)(fb.width / cell_w);
    rows = (uint32_t)(fb.height / cell_h);
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    cur_col = 0;
    cur_row = 0;
    initialized = true;

    /* Limpa a tela inteira (inclusive a borda que não cabe na grade). */
    fill_rect(0, 0, (uint32_t)fb.width, (uint32_t)fb.height, pack_color(cur_bg));
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    draw_cursor(true);
}

bool console_available(void)
{
    return initialized;
}

void console_putc(char c)
{
    if (!initialized)
        return;

    draw_cursor(false);

    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\t': {
        uint32_t next = (cur_col / TAB_WIDTH + 1) * TAB_WIDTH;
        while (cur_col < next && cur_col < cols)
            put_glyph(' ');
        break;
    }
    case '\b':
        if (cur_col > 0) {
            cur_col--;
            grid[cur_row][cur_col].ch = ' ';
            draw_cell(cur_col, cur_row, false);
        }
        break;
    default:
        if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x7F)
            c = '?';
        put_glyph(c);
        break;
    }

    draw_cursor(true);
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}

void console_clear(void)
{
    if (!initialized)
        return;
    for (uint32_t r = 0; r < rows; r++)
        clear_row(r);
    cur_col = 0;
    cur_row = 0;
    fill_rect(0, 0, (uint32_t)fb.width, (uint32_t)fb.height, pack_color(cur_bg));
    draw_cursor(true);
}

void console_set_colors(uint32_t fg, uint32_t bg)
{
    cur_fg = fg;
    cur_bg = bg;
}

void console_get_colors(uint32_t *fg, uint32_t *bg)
{
    if (fg) *fg = cur_fg;
    if (bg) *bg = cur_bg;
}

void console_get_size(uint32_t *out_cols, uint32_t *out_rows)
{
    if (out_cols) *out_cols = cols;
    if (out_rows) *out_rows = rows;
}

void console_set_cursor_visible(bool visible)
{
    if (!initialized)
        return;
    draw_cursor(false);
    cursor_visible = visible;
    draw_cursor(true);
}

const struct framebuffer *console_get_framebuffer(void)
{
    return initialized ? &fb : NULL;
}

uint32_t console_get_scale(void)
{
    return scale;
}
