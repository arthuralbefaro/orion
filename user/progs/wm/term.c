#include "term.h"

#include <string.h>

static void clear_row(struct term *t, int row)
{
    memset(t->cells[row], ' ', (size_t)t->cols);
    t->row_dirty[row] = 1;
}

void term_init(struct term *t, int cols, int rows)
{
    memset(t, 0, sizeof(*t));
    t->cols = cols > TERM_MAX_COLS ? TERM_MAX_COLS : cols;
    t->rows = rows > TERM_MAX_ROWS ? TERM_MAX_ROWS : rows;
    t->cursor_on = true;
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
    t->cells[t->cur_row][t->cur_col] = c;
    t->row_dirty[t->cur_row] = 1;
    t->cur_col++;
}

void term_putc(struct term *t, char c)
{
    if (t->esc == 1) {
        t->esc = (c == '[' || c == ']' || c == '(') ? 2 : 0;
        return;
    }
    if (t->esc == 2) {
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
        if (t->cur_col > 0) {
            t->cur_col--;
            t->cells[t->cur_row][t->cur_col] = ' ';
            t->row_dirty[t->cur_row] = 1;
        }
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
