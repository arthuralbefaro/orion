#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GUI_MAX_WIDGETS 48
#define GUI_TEXT_MAX    64
#define GUI_LIST_MAX    128
#define GUI_MENU_ITEMS  8
#define GUI_MENUS       4

#define GUI_KEY_TAB       9
#define GUI_KEY_ENTER     10
#define GUI_KEY_BACKSPACE 8
#define GUI_SC_UP         0x48
#define GUI_SC_DOWN       0x50
#define GUI_SC_PGUP       0x49
#define GUI_SC_PGDN       0x51
#define GUI_SC_LEFT       0x4B
#define GUI_SC_RIGHT      0x4D
#define GUI_SC_F10        0x44
#define GUI_KEY_ESC       27
#define GUI_MOD_EXTENDED  0x10

enum gui_kind {
    GUI_LABEL,
    GUI_BUTTON,
    GUI_TEXT,
    GUI_LIST,
    GUI_CHECK,
};

struct gui;
struct gui_widget;

typedef void (*gui_action)(struct gui *g, struct gui_widget *w);

struct gui_widget {
    enum gui_kind kind;
    int x, y, w, h;
    char text[GUI_TEXT_MAX];
    bool checked;
    const char *items[GUI_LIST_MAX];
    int count;
    int selected;
    int top;
    gui_action action;
    void *user;
};

struct gui_menu {
    const char *title;
    const char *items[GUI_MENU_ITEMS];
    int count;
    int x, w;
    void (*action)(struct gui *g, int menu, int item);
};

struct gui {
    int sock;
    int id;
    int width, height;
    uint32_t *pixels;
    char title[64];
    struct gui_widget widgets[GUI_MAX_WIDGETS];
    int count;
    int focus;
    struct gui_menu menus[GUI_MENUS];
    int menu_count;
    int open_menu;
    int menu_item;
    bool running;
    char in[512];
    int inlen;
};

bool gui_open(struct gui *g, int width, int height, const char *title);
void gui_close(struct gui *g);
void gui_set_title(struct gui *g, const char *fmt, ...);

struct gui_widget *gui_label(struct gui *g, int x, int y, int w, const char *text);
struct gui_widget *gui_button(struct gui *g, int x, int y, int w, const char *text, gui_action action);
struct gui_widget *gui_text(struct gui *g, int x, int y, int w, const char *initial, gui_action on_enter);
struct gui_widget *gui_list(struct gui *g, int x, int y, int w, int rows, gui_action on_select);
struct gui_widget *gui_check(struct gui *g, int x, int y, int w, const char *text, gui_action on_toggle);
void gui_list_add(struct gui_widget *list, const char *item);
void gui_list_clear(struct gui_widget *list);
int gui_menu(struct gui *g, const char *title, const char *const *items, int count,
             void (*action)(struct gui *g, int menu, int item));
void gui_set_text(struct gui_widget *w, const char *fmt, ...);
void gui_focus(struct gui *g, struct gui_widget *w);

void gui_redraw(struct gui *g);
void gui_run(struct gui *g);
void gui_quit(struct gui *g);

void gui_fill(struct gui *g, int x, int y, int w, int h, uint32_t rgb);
void gui_draw_text(struct gui *g, int x, int y, const char *s, uint32_t rgb);
int gui_text_width(const char *s);
