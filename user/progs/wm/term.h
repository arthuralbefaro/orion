#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TERM_MAX_COLS 96
#define TERM_MAX_ROWS 40
#define TERM_TAB      8
#define TERM_MAX_PARAMS 8

#define TERM_DEFAULT_FG 7
#define TERM_DEFAULT_BG 0

struct term_cell {
    char ch;
    uint8_t fg;
    uint8_t bg;
};

struct term {
    int cols;
    int rows;
    struct term_cell cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    uint8_t row_dirty[TERM_MAX_ROWS];
    int cur_col;
    int cur_row;
    int last_col;
    int last_row;
    int esc;
    int params[TERM_MAX_PARAMS];
    int nparams;
    bool private_seq;
    uint8_t fg;
    uint8_t bg;
    bool cursor_on;
};

void term_init(struct term *t, int cols, int rows);
void term_putc(struct term *t, char c);
void term_write(struct term *t, const char *buf, int len);
void term_mark_all(struct term *t);
bool term_take_row(struct term *t, int row);
void term_sync_cursor(struct term *t);
uint32_t term_color(uint8_t index);
