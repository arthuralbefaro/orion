#include "term.h"

#include <string.h>

static const uint32_t palette[16] = {
    0x0A0E17, 0xCC3333, 0x33CC66, 0xE6C84C, 0x3366CC, 0xB050B0, 0x33B8CC, 0xC8D4E0,
    0x707070, 0xFF6666, 0x66FF99, 0xFFE680, 0x6699FF, 0xE080E0, 0x66E0FF, 0xFFFFFF,
};

uint32_t term_color(uint8_t index)
{
    return palette[index & 0x0F];
}

static void clear_row(struct term *t, int row)
{
    for (int c = 0; c < t->cols; c++) {
        t->cells[row][c].ch = ' ';
        t->cells[row][c].fg = t->fg;
        t->cells[row][c].bg = t->bg;
    }
    t->row_dirty[row] = 1;
}

void term_init(struct term *t, int cols, int rows)
{
    memset(t, 0, sizeof(*t));
    t->cols = cols > TERM_MAX_COLS ? TERM_MAX_COLS : cols;
    t->rows = rows > TERM_MAX_ROWS ? TERM_MAX_ROWS : rows;
    t->cursor_on = true;
    t->fg = TERM_DEFAULT_FG;
    t->bg = TERM_DEFAULT_BG;
    for (int r = 0; r < t->rows; r++)
        clear_row(t, r);
}

void term_mark_all(struct term *t)
{
    for (int r = 0; r < t->rows; r++)
        t->row_dirty[r] = 1;
}

bool term_take_row(struct term *t, int row)
{
    if (row < 0 || row >= t->rows || !t->row_dirty[row])
        return false;
    t->row_dirty[row] = 0;
    return true;
}

void term_sync_cursor(struct term *t)
{
    if (t->last_row != t->cur_row || t->last_col != t->cur_col) {
        if (t->last_row >= 0 && t->last_row < t->rows)
            t->row_dirty[t->last_row] = 1;
        t->row_dirty[t->cur_row] = 1;
        t->last_row = t->cur_row;
        t->last_col = t->cur_col;
    }
}

void term_resize(struct term *t, int cols, int rows)
{
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;
    if (cols > TERM_MAX_COLS)
        cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS)
        rows = TERM_MAX_ROWS;
    int keep = t->rows;
    if (t->cur_row >= rows) {
        int drop = t->cur_row - rows + 1;
        memmove(t->cells[0], t->cells[drop], sizeof(t->cells[0]) * (size_t)(t->rows - drop));
        t->cur_row -= drop;
        keep -= drop;
    }
    for (int r = 0; r < rows; r++) {
        int from = r < keep ? t->cols : 0;
        for (int c = from; c < cols; c++) {
            t->cells[r][c].ch = ' ';
            t->cells[r][c].fg = TERM_DEFAULT_FG;
            t->cells[r][c].bg = TERM_DEFAULT_BG;
        }
    }
    t->cols = cols;
    t->rows = rows;
    if (t->cur_col > cols)
        t->cur_col = cols;
    t->last_row = t->cur_row;
    t->last_col = t->cur_col;
    term_mark_all(t);
}

static void scroll(struct term *t)
{
    memmove(t->cells[0], t->cells[1], sizeof(t->cells[0]) * (size_t)(t->rows - 1));
    clear_row(t, t->rows - 1);
    term_mark_all(t);
}

static void newline(struct term *t)
{
    t->cur_col = 0;
    t->cur_row++;
    if (t->cur_row >= t->rows) {
        t->cur_row = t->rows - 1;
        scroll(t);
    }
}

static void put_glyph(struct term *t, char c)
{
    if (t->cur_col >= t->cols)
        newline(t);
    t->cells[t->cur_row][t->cur_col].ch = c;
    t->cells[t->cur_row][t->cur_col].fg = t->fg;
    t->cells[t->cur_row][t->cur_col].bg = t->bg;
    t->row_dirty[t->cur_row] = 1;
    t->cur_col++;
}

static void clamp_cursor(struct term *t)
{
    if (t->cur_col < 0) t->cur_col = 0;
    if (t->cur_row < 0) t->cur_row = 0;
    if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
    if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
}

static void erase_range(struct term *t, int row, int from, int to)
{
    for (int c = from; c < to && c < t->cols; c++) {
        t->cells[row][c].ch = ' ';
        t->cells[row][c].fg = t->fg;
        t->cells[row][c].bg = t->bg;
    }
    t->row_dirty[row] = 1;
}

static void apply_sgr(struct term *t)
{
    if (t->nparams == 0) {
        t->fg = TERM_DEFAULT_FG;
        t->bg = TERM_DEFAULT_BG;
        return;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->fg = TERM_DEFAULT_FG;
            t->bg = TERM_DEFAULT_BG;
        } else if (p == 1) {
            t->fg |= 8;
        } else if (p == 22) {
            t->fg &= 7;
        } else if (p == 7) {
            uint8_t tmp = t->fg;
            t->fg = t->bg;
            t->bg = tmp;
        } else if (p >= 30 && p <= 37) {
            t->fg = (uint8_t)((t->fg & 8) | (p - 30));
        } else if (p == 39) {
            t->fg = TERM_DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            t->bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            t->bg = TERM_DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            t->fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            t->bg = (uint8_t)(p - 100 + 8);
        }
    }
}

static void run_csi(struct term *t, char final)
{
    int a = t->nparams > 0 ? t->params[0] : 0;
    int b = t->nparams > 1 ? t->params[1] : 0;
    int n = a > 0 ? a : 1;

    switch (final) {
    case 'A': t->cur_row -= n; break;
    case 'B': t->cur_row += n; break;
    case 'C': t->cur_col += n; break;
    case 'D': t->cur_col -= n; break;
    case 'E': t->cur_row += n; t->cur_col = 0; break;
    case 'F': t->cur_row -= n; t->cur_col = 0; break;
    case 'G': t->cur_col = (a > 0 ? a : 1) - 1; break;
    case 'H':
    case 'f':
        t->cur_row = (a > 0 ? a : 1) - 1;
        t->cur_col = (b > 0 ? b : 1) - 1;
        break;
    case 'J':
        if (a == 2 || a == 3) {
            for (int r = 0; r < t->rows; r++)
                clear_row(t, r);
            if (a == 2)
                t->cur_row = t->cur_col = 0;
        } else if (a == 1) {
            for (int r = 0; r < t->cur_row; r++)
                clear_row(t, r);
            erase_range(t, t->cur_row, 0, t->cur_col + 1);
        } else {
            erase_range(t, t->cur_row, t->cur_col, t->cols);
            for (int r = t->cur_row + 1; r < t->rows; r++)
                clear_row(t, r);
        }
        break;
    case 'K':
        if (a == 1)
            erase_range(t, t->cur_row, 0, t->cur_col + 1);
        else if (a == 2)
            erase_range(t, t->cur_row, 0, t->cols);
        else
            erase_range(t, t->cur_row, t->cur_col, t->cols);
        break;
    case 'm':
        apply_sgr(t);
        break;
    case 'h':
    case 'l':
        if (t->private_seq && a == 25)
            t->cursor_on = final == 'h';
        break;
    default:
        break;
    }
    clamp_cursor(t);
}

void term_putc(struct term *t, char c)
{
    if (t->esc == 1) {
        if (c == '[') {
            t->esc = 2;
            t->nparams = 0;
            t->params[0] = 0;
            t->private_seq = false;
        } else if (c == ']' || c == '(') {
            t->esc = 3;
        } else {
            t->esc = 0;
        }
        return;
    }
    if (t->esc == 2) {
        if (c == '?') {
            t->private_seq = true;
            return;
        }
        if (c >= '0' && c <= '9') {
            if (t->nparams == 0)
                t->nparams = 1;
            t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (t->nparams < TERM_MAX_PARAMS)
                t->params[t->nparams++] = 0;
            return;
        }
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
            run_csi(t, c);
            t->esc = 0;
        }
        return;
    }
    if (t->esc == 3) {
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E)
            t->esc = 0;
        return;
    }

    switch (c) {
    case 0x1B:
        t->esc = 1;
        break;
    case '\n':
        newline(t);
        break;
    case '\r':
        t->cur_col = 0;
        break;
    case '\b':
        if (t->cur_col > 0)
            t->cur_col--;
        break;
    case '\t': {
        int next = (t->cur_col / TERM_TAB + 1) * TERM_TAB;
        while (t->cur_col < next && t->cur_col < t->cols)
            put_glyph(t, ' ');
        break;
    }
    case '\a':
    case '\0':
        break;
    default:
        if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
            put_glyph(t, c);
        break;
    }
}

void term_write(struct term *t, const char *buf, int len)
{
    for (int i = 0; i < len; i++)
        term_putc(t, buf[i]);
    term_sync_cursor(t);
}
