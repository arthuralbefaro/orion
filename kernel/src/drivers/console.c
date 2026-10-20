/*
 * console.c - console de texto sobre framebuffer.
 */
#include <drivers/console.h>
#include <drivers/font8x8.h>
#include <lib/printf.h>
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
static bool     graphics_mode = false;

#define CONSOLE_MAX_PARAMS 8
static uint32_t default_fg = COLOR_LIGHT_GRAY;
static uint32_t default_bg = COLOR_BACKGROUND;
static uint32_t params[CONSOLE_MAX_PARAMS];
static int      nparams;
static int      esc_state;
static bool     private_seq;

static const uint32_t ansi_palette[16] = {
    0x101418, 0xCC3333, 0x33CC66, 0xE6C84C, 0x3366CC, 0xB050B0, 0x33B8CC, 0xC8D4E0,
    0x707070, 0xFF6666, 0x66FF99, 0xFFE680, 0x6699FF, 0xE080E0, 0x66E0FF, 0xFFFFFF,
};

static void apply_sgr(void)
{
    if (nparams == 0) {
        cur_fg = default_fg;
        cur_bg = default_bg;
        return;
    }
    for (int i = 0; i < nparams; i++) {
        uint32_t p = params[i];
        if (p == 0) {
            cur_fg = default_fg;
            cur_bg = default_bg;
        } else if (p == 7) {
            uint32_t tmp = cur_fg;
            cur_fg = cur_bg;
            cur_bg = tmp;
        } else if (p >= 30 && p <= 37) {
            cur_fg = ansi_palette[p - 30];
        } else if (p == 39) {
            cur_fg = default_fg;
        } else if (p >= 40 && p <= 47) {
            cur_bg = ansi_palette[p - 40];
        } else if (p == 49) {
            cur_bg = default_bg;
        } else if (p >= 90 && p <= 97) {
            cur_fg = ansi_palette[p - 90 + 8];
        } else if (p >= 100 && p <= 107) {
            cur_bg = ansi_palette[p - 100 + 8];
        }
    }
}

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
    if (graphics_mode)
        return;
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
    if (!cursor_visible || graphics_mode)
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

static void set_geometry(void)
{
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
}

void console_init(const struct framebuffer *info)
{
    if (!info || !info->address || info->bpp != 32)
        return;

    fb = *info;
    set_geometry();

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

bool console_set_framebuffer(const struct framebuffer *info)
{
    if (!info || !info->address || info->bpp != 32 || !initialized)
        return false;
    uint64_t flags;
    bool locked = kprint_lock(&flags);
    uint32_t old_cols = cols;
    uint32_t old_rows = rows;
    fb = *info;
    set_geometry();
    for (uint32_t r = old_rows; r < rows; r++)
        clear_row(r);
    if (cur_row >= rows) {
        uint32_t shift = cur_row - rows + 1;
        memmove(&grid[0], &grid[shift], sizeof(grid[0]) * (MAX_ROWS - shift));
        cur_row = rows - 1;
    }
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = old_cols; c < cols; c++) {
            grid[r][c].ch = ' ';
            grid[r][c].fg = cur_fg;
            grid[r][c].bg = cur_bg;
        }
    }
    if (cur_col >= cols)
        cur_col = cols - 1;
    if (!graphics_mode) {
        fill_rect(0, 0, (uint32_t)fb.width, (uint32_t)fb.height, pack_color(cur_bg));
        redraw_all();
        draw_cursor(true);
    }
    kprint_unlock(locked, flags);
    return true;
}

static void erase_range(uint32_t row, uint32_t from, uint32_t to)
{
    for (uint32_t c = from; c < to && c < cols; c++) {
        grid[row][c].ch = ' ';
        grid[row][c].fg = cur_fg;
        grid[row][c].bg = cur_bg;
        draw_cell(c, row, false);
    }
}

static void clamp_cursor(void)
{
    if (cur_col >= cols)
        cur_col = cols - 1;
    if (cur_row >= rows)
        cur_row = rows - 1;
}

static void move_up(uint32_t n)
{
    cur_row = cur_row > n ? cur_row - n : 0;
}

static void move_left(uint32_t n)
{
    cur_col = cur_col > n ? cur_col - n : 0;
}

static void run_csi(char final)
{
    uint32_t a = nparams > 0 ? params[0] : 0;
    uint32_t b = nparams > 1 ? params[1] : 0;
    uint32_t n = a > 0 ? a : 1;

    switch (final) {
    case 'A': move_up(n); break;
    case 'B': cur_row += n; break;
    case 'C': cur_col += n; break;
    case 'D': move_left(n); break;
    case 'E': cur_row += n; cur_col = 0; break;
    case 'F': move_up(n); cur_col = 0; break;
    case 'G': cur_col = (a > 0 ? a : 1) - 1; break;
    case 'H':
    case 'f':
        cur_row = (a > 0 ? a : 1) - 1;
        cur_col = (b > 0 ? b : 1) - 1;
        break;
    case 'J':
        if (a == 2 || a == 3) {
            for (uint32_t r = 0; r < rows; r++)
                clear_row(r);
            redraw_all();
            if (a == 2)
                cur_row = cur_col = 0;
        } else if (a == 1) {
            for (uint32_t r = 0; r < cur_row; r++)
                erase_range(r, 0, cols);
            erase_range(cur_row, 0, cur_col + 1);
        } else {
            erase_range(cur_row, cur_col, cols);
            for (uint32_t r = cur_row + 1; r < rows; r++)
                erase_range(r, 0, cols);
        }
        break;
    case 'K':
        if (a == 1)
            erase_range(cur_row, 0, cur_col + 1);
        else if (a == 2)
            erase_range(cur_row, 0, cols);
        else
            erase_range(cur_row, cur_col, cols);
        break;
    case 'm':
        apply_sgr();
        break;
    case 'h':
    case 'l':
        if (private_seq && a == 25)
            cursor_visible = final == 'h';
        break;
    default:
        break;
    }
    clamp_cursor();
}

static bool consume_escape(char c)
{
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            nparams = 0;
            params[0] = 0;
            private_seq = false;
        } else if (c == ']' || c == '(') {
            esc_state = 3;
        } else {
            esc_state = 0;
        }
        return true;
    }
    if (esc_state == 2) {
        if (c == '?') {
            private_seq = true;
            return true;
        }
        if (c >= '0' && c <= '9') {
            if (nparams == 0)
                nparams = 1;
            params[nparams - 1] = params[nparams - 1] * 10 + (uint32_t)(c - '0');
            return true;
        }
        if (c == ';') {
            if (nparams < CONSOLE_MAX_PARAMS)
                params[nparams++] = 0;
            return true;
        }
        if ((uint8_t)c >= 0x40 && (uint8_t)c <= 0x7E) {
            run_csi(c);
            esc_state = 0;
        }
        return true;
    }
    if (esc_state == 3) {
        if ((uint8_t)c >= 0x40 && (uint8_t)c <= 0x7E)
            esc_state = 0;
        return true;
    }
    if (c == 0x1B) {
        esc_state = 1;
        return true;
    }
    return false;
}

void console_putc(char c)
{
    if (!initialized)
        return;

    if (esc_state || c == 0x1B) {
        draw_cursor(false);
        consume_escape(c);
        draw_cursor(true);
        return;
    }

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
    if (!graphics_mode)
        fill_rect(0, 0, (uint32_t)fb.width, (uint32_t)fb.height, pack_color(cur_bg));
    draw_cursor(true);
}

void console_redraw(void)
{
    if (!initialized)
        return;
    graphics_mode = false;
    fill_rect(0, 0, (uint32_t)fb.width, (uint32_t)fb.height, pack_color(cur_bg));
    redraw_all();
    draw_cursor(true);
}

void console_set_graphics(bool on)
{
    if (graphics_mode == on)
        return;
    if (on)
        graphics_mode = true;
    else
        console_redraw();
}

bool console_graphics(void)
{
    return graphics_mode;
}

void console_set_colors(uint32_t fg, uint32_t bg)
{
    default_fg = fg;
    default_bg = bg;
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
