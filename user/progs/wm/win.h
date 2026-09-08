#pragma once

#include "gfx.h"
#include "term.h"

#define WIN_MAX       8
#define WIN_TITLE_H   22
#define WIN_BORDER    2
#define WIN_PAD       4
#define WIN_CHROME_X  (2 * (WIN_BORDER + WIN_PAD))
#define WIN_CHROME_Y  (2 * (WIN_BORDER + WIN_PAD) + WIN_TITLE_H)
#define TOPBAR_H      28

enum win_kind {
    WIN_TERM,
    WIN_CLOCK,
    WIN_ABOUT,
    WIN_PROCS,
    WIN_FILES,
};

#define PANEL_ROWS 16
#define PANEL_COLS 64
#define MENU_ITEMS 5
#define MENU_W     260

struct window {
    bool used;
    int id;
    enum win_kind kind;
    struct rect frame;
    char title[32];
    struct term term;
    int master;
    int pid;
    char rows_text[PANEL_ROWS][PANEL_COLS];
    int  nrows;
    int  selected;
    char path[128];
};

struct desktop {
    struct gfx *gfx;
    struct window windows[WIN_MAX];
    int order[WIN_MAX];
    int count;
    char clock[24];
    char date[24];
    char ip[24];
    char mem[32];
    const char *hint;
    bool menu_open;
    int  menu_hover;
};

void desk_init(struct desktop *d, struct gfx *g);
struct window *desk_new(struct desktop *d, enum win_kind kind, const char *title, struct rect frame);
void desk_remove(struct desktop *d, struct window *w);
struct window *desk_focused(struct desktop *d);
struct window *desk_at(struct desktop *d, int x, int y);
void desk_raise(struct desktop *d, struct window *w);
void desk_move(struct desktop *d, struct window *w, int x, int y);

struct rect win_content(const struct window *w);
struct rect win_titlebar(const struct window *w);
struct rect win_close_button(const struct window *w);
struct rect win_row_rect(const struct window *w, int row);

void desk_damage_window(struct desktop *d, const struct window *w);
void desk_damage_term_rows(struct desktop *d, struct window *w);
void desk_damage_topbar(struct desktop *d);
void desk_damage_menu(struct desktop *d);
struct rect desk_menu_rect(const struct desktop *d);
struct rect desk_menu_button(const struct desktop *d);
int  desk_menu_item_at(const struct desktop *d, int x, int y);
const char *desk_menu_label(int i);
void panel_set(struct window *w, int row, const char *text);
void desk_compose(struct desktop *d, struct rect area);
