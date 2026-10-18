#include "gui.h"
#include "font8x8.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WM_SOCKET  "/run/wmapp.sock"
#define ZOOM       2
#define CHAR_W     (FONT_WIDTH * ZOOM)
#define CHAR_H     (FONT_HEIGHT * ZOOM)
#define ROW_H      20
#define FIELD_H    24
#define MENUBAR_H  24
#define SCROLL_W   18

#define COL_BG      0x1B2230
#define COL_PANEL   0x242D3E
#define COL_TEXT    0xE8ECF4
#define COL_DIM     0x8C96AA
#define COL_ACCENT  0x5CA8FF
#define COL_BUTTON  0x34405A
#define COL_FIELD   0xF4F6FA
#define COL_FIELD_T 0x101418
#define COL_SELECT  0x2F6FD0
#define COL_FOCUS   0xFFD060
#define COL_MENU    0x2A3346

static void send_line(struct gui *g, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(line))
        n = (int)sizeof(line) - 1;
    const char *p = line;
    while (n > 0) {
        ssize_t w = write(g->sock, p, (size_t)n);
        if (w <= 0)
            return;
        p += w;
        n -= (int)w;
    }
}

static int read_line(struct gui *g, char *out, size_t size)
{
    for (;;) {
        char *nl = memchr(g->in, '\n', (size_t)g->inlen);
        if (nl) {
            size_t len = (size_t)(nl - g->in);
            if (len >= size)
                len = size - 1;
            memcpy(out, g->in, len);
            out[len] = 0;
            int used = (int)(nl - g->in) + 1;
            memmove(g->in, g->in + used, (size_t)(g->inlen - used));
            g->inlen -= used;
            return 1;
        }
        if (g->inlen >= (int)sizeof(g->in) - 1)
            return -1;
        ssize_t n = read(g->sock, g->in + g->inlen, sizeof(g->in) - 1 - (size_t)g->inlen);
        if (n <= 0)
            return 0;
        g->inlen += (int)n;
    }
}

bool gui_open(struct gui *g, int width, int height, const char *title)
{
    memset(g, 0, sizeof(*g));
    signal(SIGPIPE, SIG_IGN);
    g->focus = -1;
    g->open_menu = -1;
    g->sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WM_SOCKET);
    if (g->sock < 0 || connect(g->sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "libgui: %s: %s (o wm esta rodando?)\n", WM_SOCKET, strerror(errno));
        return false;
    }
    snprintf(g->title, sizeof(g->title), "%s", title);
    send_line(g, "janela %d %d %s\n", width, height, title);
    char line[256], path[128];
    int w = 0, h = 0;
    if (read_line(g, line, sizeof(line)) <= 0 || sscanf(line, "ok %d %127s %d %d", &g->id, path, &w, &h) != 4) {
        fprintf(stderr, "libgui: o wm recusou a janela: %s\n", line);
        close(g->sock);
        return false;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    void *m = fd >= 0 ? mmap(NULL, (size_t)w * (size_t)h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (fd >= 0)
        close(fd);
    if (m == MAP_FAILED) {
        fprintf(stderr, "libgui: %s: %s\n", path, strerror(errno));
        close(g->sock);
        return false;
    }
    g->pixels = m;
    g->width = w;
    g->height = h;
    g->running = true;
    return true;
}

void gui_close(struct gui *g)
{
    if (g->sock >= 0) {
        send_line(g, "fecha\n");
        close(g->sock);
        g->sock = -1;
    }
    if (g->pixels) {
        munmap(g->pixels, (size_t)g->width * (size_t)g->height * 4);
        g->pixels = NULL;
    }
}

void gui_set_title(struct gui *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->title, sizeof(g->title), fmt, ap);
    va_end(ap);
    send_line(g, "titulo %s\n", g->title);
}

void gui_fill(struct gui *g, int x, int y, int w, int h, uint32_t rgb)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > g->width ? g->width : x + w;
    int y1 = y + h > g->height ? g->height : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = g->pixels + (size_t)j * (size_t)g->width;
        for (int i = x0; i < x1; i++)
            row[i] = rgb;
    }
}

static void frame(struct gui *g, int x, int y, int w, int h, uint32_t rgb)
{
    gui_fill(g, x, y, w, 1, rgb);
    gui_fill(g, x, y + h - 1, w, 1, rgb);
    gui_fill(g, x, y, 1, h, rgb);
    gui_fill(g, x + w - 1, y, 1, h, rgb);
}

static void draw_char(struct gui *g, int x, int y, unsigned char c, uint32_t rgb)
{
    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR)
        c = '?';
    const uint8_t *glyph = font8x8_basic[c - FONT_FIRST_CHAR];
    for (int gy = 0; gy < FONT_HEIGHT; gy++)
        for (int gx = 0; gx < FONT_WIDTH; gx++)
            if ((glyph[gy] >> gx) & 1)
                gui_fill(g, x + gx * ZOOM, y + gy * ZOOM, ZOOM, ZOOM, rgb);
}

void gui_draw_text(struct gui *g, int x, int y, const char *s, uint32_t rgb)
{
    for (; *s; s++, x += CHAR_W)
        draw_char(g, x, y, (unsigned char)*s, rgb);
}

static void draw_text_clip(struct gui *g, int x, int y, const char *s, int max_chars, uint32_t rgb)
{
    for (int i = 0; s[i] && i < max_chars; i++)
        draw_char(g, x + i * CHAR_W, y, (unsigned char)s[i], rgb);
}

int gui_text_width(const char *s)
{
    return (int)strlen(s) * CHAR_W;
}

static struct gui_widget *add(struct gui *g, enum gui_kind kind, int x, int y, int w, int h, const char *text)
{
    if (g->count >= GUI_MAX_WIDGETS)
        return NULL;
    struct gui_widget *wd = &g->widgets[g->count++];
    memset(wd, 0, sizeof(*wd));
    wd->kind = kind;
    wd->x = x;
    wd->y = y;
    wd->w = w;
    wd->h = h;
    wd->selected = -1;
    snprintf(wd->text, sizeof(wd->text), "%s", text ? text : "");
    if (g->focus < 0 && kind != GUI_LABEL)
        g->focus = g->count - 1;
    return wd;
}

struct gui_widget *gui_label(struct gui *g, int x, int y, int w, const char *text)
{
    return add(g, GUI_LABEL, x, y, w, ROW_H, text);
}

struct gui_widget *gui_button(struct gui *g, int x, int y, int w, const char *text, gui_action action)
{
    struct gui_widget *b = add(g, GUI_BUTTON, x, y, w, FIELD_H, text);
    if (b)
        b->action = action;
    return b;
}

struct gui_widget *gui_text(struct gui *g, int x, int y, int w, const char *initial, gui_action on_enter)
{
    struct gui_widget *t = add(g, GUI_TEXT, x, y, w, FIELD_H, initial);
    if (t)
        t->action = on_enter;
    return t;
}

struct gui_widget *gui_list(struct gui *g, int x, int y, int w, int rows, gui_action on_select)
{
    struct gui_widget *l = add(g, GUI_LIST, x, y, w, rows * ROW_H + 4, NULL);
    if (l)
        l->action = on_select;
    return l;
}

struct gui_widget *gui_check(struct gui *g, int x, int y, int w, const char *text, gui_action on_toggle)
{
    struct gui_widget *c = add(g, GUI_CHECK, x, y, w, FIELD_H, text);
    if (c)
        c->action = on_toggle;
    return c;
}

void gui_list_add(struct gui_widget *list, const char *item)
{
    if (list->count < GUI_LIST_MAX)
        list->items[list->count++] = item;
}

void gui_list_clear(struct gui_widget *list)
{
    list->count = 0;
    list->selected = -1;
    list->top = 0;
}

int gui_menu(struct gui *g, const char *title, const char *const *items, int count,
             void (*action)(struct gui *g, int menu, int item))
{
    if (g->menu_count >= GUI_MENUS)
        return -1;
    struct gui_menu *m = &g->menus[g->menu_count];
    m->title = title;
    m->count = count < GUI_MENU_ITEMS ? count : GUI_MENU_ITEMS;
    for (int i = 0; i < m->count; i++)
        m->items[i] = items[i];
    m->action = action;
    m->x = g->menu_count ? g->menus[g->menu_count - 1].x + g->menus[g->menu_count - 1].w : 0;
    m->w = gui_text_width(title) + 20;
    return g->menu_count++;
}

void gui_set_text(struct gui_widget *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->text, sizeof(w->text), fmt, ap);
    va_end(ap);
}

void gui_focus(struct gui *g, struct gui_widget *w)
{
    g->focus = (int)(w - g->widgets);
}

static int list_rows(const struct gui_widget *l)
{
    return (l->h - 4) / ROW_H;
}

static void list_show(struct gui_widget *l)
{
    int rows = list_rows(l);
    if (l->selected < l->top)
        l->top = l->selected;
    if (l->selected >= l->top + rows)
        l->top = l->selected - rows + 1;
    if (l->top > l->count - rows)
        l->top = l->count - rows;
    if (l->top < 0)
        l->top = 0;
}

static void draw_widget(struct gui *g, struct gui_widget *w, bool focused)
{
    int ty = w->y + (w->h - CHAR_H) / 2;
    switch (w->kind) {
    case GUI_LABEL:
        draw_text_clip(g, w->x, w->y + 2, w->text, w->w / CHAR_W, COL_TEXT);
        break;
    case GUI_BUTTON: {
        gui_fill(g, w->x, w->y, w->w, w->h, COL_BUTTON);
        frame(g, w->x, w->y, w->w, w->h, focused ? COL_FOCUS : COL_ACCENT);
        int tw = gui_text_width(w->text);
        draw_text_clip(g, w->x + (w->w - tw) / 2, ty, w->text, (w->w - 4) / CHAR_W, COL_TEXT);
        break;
    }
    case GUI_TEXT: {
        gui_fill(g, w->x, w->y, w->w, w->h, COL_FIELD);
        frame(g, w->x, w->y, w->w, w->h, focused ? COL_FOCUS : COL_DIM);
        int room = (w->w - 8) / CHAR_W;
        int len = (int)strlen(w->text);
        const char *shown = len > room - 1 ? w->text + (len - room + 1) : w->text;
        draw_text_clip(g, w->x + 4, ty, shown, room, COL_FIELD_T);
        if (focused) {
            int cx = w->x + 4 + (int)strlen(shown) * CHAR_W;
            gui_fill(g, cx, w->y + 3, 2, w->h - 6, COL_FIELD_T);
        }
        break;
    }
    case GUI_CHECK:
        gui_fill(g, w->x, w->y + 3, 18, 18, COL_FIELD);
        frame(g, w->x, w->y + 3, 18, 18, focused ? COL_FOCUS : COL_DIM);
        if (w->checked)
            gui_fill(g, w->x + 4, w->y + 7, 10, 10, COL_SELECT);
        draw_text_clip(g, w->x + 26, ty, w->text, (w->w - 26) / CHAR_W, COL_TEXT);
        break;
    case GUI_LIST: {
        gui_fill(g, w->x, w->y, w->w, w->h, COL_PANEL);
        frame(g, w->x, w->y, w->w, w->h, focused ? COL_FOCUS : COL_DIM);
        int rows = list_rows(w);
        int inner = w->w - SCROLL_W - 4;
        for (int r = 0; r < rows && w->top + r < w->count; r++) {
            int i = w->top + r;
            int ry = w->y + 2 + r * ROW_H;
            if (i == w->selected)
                gui_fill(g, w->x + 2, ry, inner, ROW_H, COL_SELECT);
            draw_text_clip(g, w->x + 6, ry + 2, w->items[i], (inner - 8) / CHAR_W, COL_TEXT);
        }
        int sx = w->x + w->w - SCROLL_W - 1;
        gui_fill(g, sx, w->y + 1, SCROLL_W, w->h - 2, COL_BG);
        gui_fill(g, sx, w->y + 1, SCROLL_W, SCROLL_W, COL_BUTTON);
        gui_fill(g, sx, w->y + w->h - 1 - SCROLL_W, SCROLL_W, SCROLL_W, COL_BUTTON);
        draw_char(g, sx + 1, w->y + 2, '^', COL_TEXT);
        draw_char(g, sx + 1, w->y + w->h - SCROLL_W, 'v', COL_TEXT);
        int track = w->h - 2 - 2 * SCROLL_W;
        if (w->count > rows && track > 8) {
            int thumb = track * rows / w->count;
            if (thumb < 8)
                thumb = 8;
            int pos = (track - thumb) * w->top / (w->count - rows);
            gui_fill(g, sx + 3, w->y + 1 + SCROLL_W + pos, SCROLL_W - 6, thumb, COL_ACCENT);
        }
        break;
    }
    }
}

static void draw_menus(struct gui *g)
{
    if (!g->menu_count)
        return;
    gui_fill(g, 0, 0, g->width, MENUBAR_H, COL_MENU);
    for (int i = 0; i < g->menu_count; i++) {
        struct gui_menu *m = &g->menus[i];
        if (i == g->open_menu)
            gui_fill(g, m->x, 0, m->w, MENUBAR_H, COL_SELECT);
        gui_draw_text(g, m->x + 10, 4, m->title, COL_TEXT);
    }
    if (g->open_menu < 0)
        return;
    struct gui_menu *m = &g->menus[g->open_menu];
    int w = 0;
    for (int i = 0; i < m->count; i++)
        if (gui_text_width(m->items[i]) > w)
            w = gui_text_width(m->items[i]);
    w += 24;
    gui_fill(g, m->x, MENUBAR_H, w, m->count * ROW_H + 4, COL_MENU);
    frame(g, m->x, MENUBAR_H, w, m->count * ROW_H + 4, COL_ACCENT);
    for (int i = 0; i < m->count; i++) {
        if (i == g->menu_item)
            gui_fill(g, m->x + 2, MENUBAR_H + 2 + i * ROW_H, w - 4, ROW_H, COL_SELECT);
        gui_draw_text(g, m->x + 10, MENUBAR_H + 4 + i * ROW_H, m->items[i], COL_TEXT);
    }
}

static uint32_t widget_sig(const struct gui *g, const struct gui_widget *w, int index)
{
    uint32_t h = 2166136261u;
    for (const char *c = w->text; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    int vals[] = { w->checked, w->selected, w->top, w->count, index == g->focus };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
        h = (h ^ (uint32_t)vals[i]) * 16777619u;
    return h | 1;
}

void gui_redraw(struct gui *g)
{
    int menu_sig = g->open_menu * 64 + g->menu_item;
    if (!g->drawn || menu_sig != g->drawn_menu) {
        gui_fill(g, 0, 0, g->width, g->height, COL_BG);
        for (int i = 0; i < g->count; i++) {
            draw_widget(g, &g->widgets[i], i == g->focus);
            g->widgets[i].drawn_sig = widget_sig(g, &g->widgets[i], i);
        }
        draw_menus(g);
        g->drawn = true;
        g->drawn_menu = menu_sig;
        g->frames++;
        send_line(g, "mostra\n");
        return;
    }
    int x0 = g->width, y0 = g->height, x1 = 0, y1 = 0;
    for (int i = 0; i < g->count; i++) {
        struct gui_widget *w = &g->widgets[i];
        uint32_t sig = widget_sig(g, w, i);
        if (sig == w->drawn_sig)
            continue;
        gui_fill(g, w->x, w->y, w->w, w->h, COL_BG);
        draw_widget(g, w, i == g->focus);
        w->drawn_sig = sig;
        if (w->x < x0)
            x0 = w->x;
        if (w->y < y0)
            y0 = w->y;
        if (w->x + w->w > x1)
            x1 = w->x + w->w;
        if (w->y + w->h > y1)
            y1 = w->y + w->h;
    }
    if (x1 <= x0 || y1 <= y0)
        return;
    g->frames++;
    g->partial_frames++;
    send_line(g, "mostra %d %d %d %d\n", x0, y0, x1 - x0, y1 - y0);
}

static void activate(struct gui *g, struct gui_widget *w)
{
    if (w->kind == GUI_CHECK)
        w->checked = !w->checked;
    if (w->action)
        w->action(g, w);
}

static void focus_next(struct gui *g, int dir)
{
    if (!g->count)
        return;
    int i = g->focus;
    for (int n = 0; n < g->count; n++) {
        i = (i + dir + g->count) % g->count;
        if (g->widgets[i].kind != GUI_LABEL) {
            g->focus = i;
            return;
        }
    }
}

static void key(struct gui *g, int ascii, int scancode, int mods)
{
    bool ext = (mods & GUI_MOD_EXTENDED) != 0;
    if (!ext && scancode == GUI_SC_F10 && g->menu_count) {
        g->open_menu = g->open_menu >= 0 ? -1 : 0;
        g->menu_item = 0;
        return;
    }
    if (g->open_menu >= 0) {
        struct gui_menu *m = &g->menus[g->open_menu];
        if (ascii == GUI_KEY_ESC) {
            g->open_menu = -1;
        } else if (ext && (scancode == GUI_SC_LEFT || scancode == GUI_SC_RIGHT)) {
            int d = scancode == GUI_SC_LEFT ? -1 : 1;
            g->open_menu = (g->open_menu + d + g->menu_count) % g->menu_count;
            g->menu_item = 0;
        } else if (ext && (scancode == GUI_SC_UP || scancode == GUI_SC_DOWN) && m->count) {
            int d = scancode == GUI_SC_UP ? -1 : 1;
            g->menu_item = (g->menu_item + d + m->count) % m->count;
        } else if (!ext && ascii == GUI_KEY_ENTER) {
            int menu = g->open_menu, item = g->menu_item;
            g->open_menu = -1;
            if (m->action && item >= 0 && item < m->count)
                m->action(g, menu, item);
        }
        return;
    }
    if (ascii == GUI_KEY_TAB) {
        focus_next(g, 1);
        return;
    }
    if (g->focus < 0)
        return;
    struct gui_widget *w = &g->widgets[g->focus];
    if (w->kind == GUI_LIST && ext) {
        int rows = list_rows(w);
        int step = scancode == GUI_SC_UP ? -1 : scancode == GUI_SC_DOWN ? 1
                 : scancode == GUI_SC_PGUP ? -rows : scancode == GUI_SC_PGDN ? rows : 0;
        if (!step || !w->count)
            return;
        int s = w->selected < 0 ? 0 : w->selected + step;
        w->selected = s < 0 ? 0 : s >= w->count ? w->count - 1 : s;
        list_show(w);
        if (w->action)
            w->action(g, w);
        return;
    }
    if (ext)
        return;
    if (w->kind == GUI_TEXT) {
        size_t len = strlen(w->text);
        if (ascii == GUI_KEY_BACKSPACE) {
            if (len)
                w->text[len - 1] = 0;
        } else if (ascii == GUI_KEY_ENTER) {
            if (w->action)
                w->action(g, w);
        } else if (ascii >= 32 && ascii < 127 && len + 1 < sizeof(w->text)) {
            w->text[len] = (char)ascii;
            w->text[len + 1] = 0;
        }
        return;
    }
    if (ascii == GUI_KEY_ENTER || ascii == ' ')
        activate(g, w);
}

static bool inside(const struct gui_widget *w, int x, int y)
{
    return x >= w->x && y >= w->y && x < w->x + w->w && y < w->y + w->h;
}

static bool menu_click(struct gui *g, int x, int y)
{
    if (!g->menu_count)
        return false;
    if (g->open_menu >= 0) {
        struct gui_menu *m = &g->menus[g->open_menu];
        int item = (y - MENUBAR_H - 2) / ROW_H;
        int w = 0;
        for (int i = 0; i < m->count; i++)
            if (gui_text_width(m->items[i]) + 24 > w)
                w = gui_text_width(m->items[i]) + 24;
        int menu = g->open_menu;
        g->open_menu = -1;
        if (x >= m->x && x < m->x + w && y >= MENUBAR_H && item >= 0 && item < m->count) {
            if (m->action)
                m->action(g, menu, item);
            return true;
        }
        if (y >= MENUBAR_H)
            return true;
    }
    if (y < MENUBAR_H) {
        for (int i = 0; i < g->menu_count; i++)
            if (x >= g->menus[i].x && x < g->menus[i].x + g->menus[i].w)
                g->open_menu = i;
        g->menu_item = -1;
        return true;
    }
    return false;
}

static void click(struct gui *g, int x, int y)
{
    if (menu_click(g, x, y))
        return;
    for (int i = 0; i < g->count; i++) {
        struct gui_widget *w = &g->widgets[i];
        if (!inside(w, x, y) || w->kind == GUI_LABEL)
            continue;
        g->focus = i;
        if (w->kind == GUI_LIST) {
            int sx = w->x + w->w - SCROLL_W - 1;
            int rows = list_rows(w);
            if (x >= sx) {
                if (y < w->y + 1 + SCROLL_W)
                    w->top = w->top > 0 ? w->top - 1 : 0;
                else if (y >= w->y + w->h - 1 - SCROLL_W && w->top < w->count - rows)
                    w->top++;
                return;
            }
            int item = w->top + (y - w->y - 2) / ROW_H;
            if (item >= 0 && item < w->count) {
                w->selected = item;
                if (w->action)
                    w->action(g, w);
            }
            return;
        }
        if (w->kind == GUI_BUTTON || w->kind == GUI_CHECK)
            activate(g, w);
        return;
    }
}

void gui_quit(struct gui *g)
{
    g->running = false;
}

void gui_run(struct gui *g)
{
    gui_redraw(g);
    char line[256];
    while (g->running) {
        int r = read_line(g, line, sizeof(line));
        if (r <= 0)
            break;
        int a, b, c;
        unsigned btn;
        if (sscanf(line, "tecla %d %d %d", &a, &b, &c) == 3)
            key(g, a, b, c);
        else if (sscanf(line, "clique %d %d %u", &a, &b, &btn) == 3)
            click(g, a, b);
        else if (strcmp(line, "fechar") == 0) {
            close(g->sock);
            g->sock = -1;
            break;
        }
        else
            continue;
        if (g->running)
            gui_redraw(g);
    }
    gui_close(g);
}
