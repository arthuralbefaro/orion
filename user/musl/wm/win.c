#include "win.h"

#include <stdio.h>
#include <string.h>

#define COL_DESK_TOP   0x0E1220
#define COL_DESK_BOT   0x1B2438
#define COL_TOPBAR     0x161C2B
#define COL_TOPBAR_ED  0x2E3950
#define COL_BORDER     0x394663
#define COL_TITLE_ON   0x2C5E9E
#define COL_TITLE_OFF  0x2A3040
#define COL_TITLE_TXT  0xE8EDF5
#define COL_CLOSE      0xC85A50
#define COL_CLOSE_OFF  0x555F73
#define COL_TERM_BG    0x0A0E17
#define COL_TERM_FG    0xC8D4E0
#define COL_PANEL_BG   0x141A28
#define COL_ACCENT     0x63B3ED
#define COL_MENU_BG    0x1B2333
#define COL_MENU_HOVER 0x2C5E9E
#define COL_SELECT     0x24446E

static const char *const menu_labels[MENU_ITEMS] = {
    "Novo terminal",
    "Processos",
    "Arquivos",
    "Sobre",
    "Encerrar o wm",
};

const char *desk_menu_label(int i)
{
    return i >= 0 && i < MENU_ITEMS ? menu_labels[i] : "";
}

struct rect desk_menu_button(const struct desktop *d)
{
    (void)d;
    return rect_make(6, 2, 10 * GFX_CELL_W + 12, TOPBAR_H - 4);
}

struct rect desk_menu_rect(const struct desktop *d)
{
    (void)d;
    return rect_make(6, TOPBAR_H, MENU_W, MENU_ITEMS * GFX_CELL_H + 8);
}

int desk_menu_item_at(const struct desktop *d, int x, int y)
{
    struct rect m = desk_menu_rect(d);
    if (!rect_contains(m, x, y))
        return -1;
    int i = (y - m.y - 4) / GFX_CELL_H;
    return i >= 0 && i < MENU_ITEMS ? i : -1;
}

void desk_damage_menu(struct desktop *d)
{
    gfx_damage(d->gfx, desk_menu_rect(d));
}

void panel_set(struct window *w, int row, const char *text)
{
    if (row < 0 || row >= PANEL_ROWS)
        return;
    snprintf(w->rows_text[row], PANEL_COLS, "%s", text);
}

static const char *const about_lines[] = {
    "Orion - compositor em espaco de usuario",
    "",
    "Ctrl+T       novo terminal      Ctrl+P   processos",
    "Ctrl+F       arquivos           Ctrl+Q   fecha a janela",
    "Ctrl+Alt+X   encerra o wm",
    "",
    "Arraste pela barra de titulo para mover.",
    "Clique numa janela para focar e trazer a frente.",
};

void desk_init(struct desktop *d, struct gfx *g)
{
    memset(d, 0, sizeof(*d));
    d->gfx = g;
    d->hint = "Ctrl+T terminal  Ctrl+P processos  Ctrl+F arquivos  Ctrl+Q fecha";
    strcpy(d->clock, "--:--:--");
    strcpy(d->date, "--/--/----");
    strcpy(d->ip, "sem rede");
    strcpy(d->mem, "");
}

struct rect win_titlebar(const struct window *w)
{
    return rect_make(w->frame.x + WIN_BORDER, w->frame.y + WIN_BORDER,
                     w->frame.w - 2 * WIN_BORDER, WIN_TITLE_H);
}

struct rect win_content(const struct window *w)
{
    return rect_make(w->frame.x + WIN_BORDER + WIN_PAD,
                     w->frame.y + WIN_BORDER + WIN_TITLE_H + WIN_PAD,
                     w->frame.w - WIN_CHROME_X, w->frame.h - WIN_CHROME_Y);
}

struct rect win_close_button(const struct window *w)
{
    struct rect t = win_titlebar(w);
    return rect_make(t.x + t.w - WIN_TITLE_H, t.y, WIN_TITLE_H, WIN_TITLE_H);
}

struct rect win_min_button(const struct window *w)
{
    struct rect c = win_close_button(w);
    return rect_make(c.x - WIN_TITLE_H, c.y, WIN_TITLE_H, WIN_TITLE_H);
}

struct rect win_max_button(const struct window *w)
{
    struct rect c = win_close_button(w);
    if (w->kind != WIN_TERM)
        return rect_make(c.x, c.y, 0, 0);
    return rect_make(c.x - 2 * WIN_TITLE_H, c.y, WIN_TITLE_H, WIN_TITLE_H);
}

struct rect win_row_rect(const struct window *w, int row)
{
    struct rect c = win_content(w);
    return rect_make(c.x, c.y + row * GFX_CELL_H, c.w, GFX_CELL_H);
}

static int index_of(const struct desktop *d, const struct window *w)
{
    return (int)(w - d->windows);
}

static void damage_focus(struct desktop *d)
{
    struct window *w = desk_focused(d);
    if (w)
        gfx_damage(d->gfx, win_titlebar(w));
    desk_damage_taskbar(d);
}

struct window *desk_new(struct desktop *d, enum win_kind kind, const char *title, struct rect frame)
{
    if (d->count >= WIN_MAX)
        return NULL;
    damage_focus(d);
    for (int i = 0; i < WIN_MAX; i++) {
        struct window *w = &d->windows[i];
        if (w->used)
            continue;
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->id = i;
        w->kind = kind;
        w->frame = frame;
        w->master = -1;
        w->pid = -1;
        snprintf(w->title, sizeof(w->title), "%s", title);
        d->order[d->count++] = i;
        return w;
    }
    return NULL;
}

void desk_remove(struct desktop *d, struct window *w)
{
    int idx = index_of(d, w);
    damage_focus(d);
    int out = 0;
    for (int i = 0; i < d->count; i++)
        if (d->order[i] != idx)
            d->order[out++] = d->order[i];
    d->count = out;
    w->used = false;
}

struct window *desk_focused(struct desktop *d)
{
    for (int i = d->count - 1; i >= 0; i--) {
        struct window *w = &d->windows[d->order[i]];
        if (!w->minimized)
            return w;
    }
    return NULL;
}

struct window *desk_at(struct desktop *d, int x, int y)
{
    for (int i = d->count - 1; i >= 0; i--) {
        struct window *w = &d->windows[d->order[i]];
        if (!w->minimized && rect_contains(w->frame, x, y))
            return w;
    }
    return NULL;
}

static void to_bottom(struct desktop *d, int idx)
{
    int out = d->count - 1;
    for (int i = d->count - 1; i >= 0; i--)
        if (d->order[i] != idx)
            d->order[out--] = d->order[i];
    d->order[0] = idx;
}

void desk_minimize(struct desktop *d, struct window *w)
{
    if (w->minimized)
        return;
    damage_focus(d);
    desk_damage_window(d, w);
    w->minimized = true;
    to_bottom(d, index_of(d, w));
    damage_focus(d);
}

void desk_restore(struct desktop *d, struct window *w)
{
    damage_focus(d);
    w->minimized = false;
    int idx = index_of(d, w);
    int out = 0;
    for (int i = 0; i < d->count; i++)
        if (d->order[i] != idx)
            d->order[out++] = d->order[i];
    d->order[out++] = idx;
    desk_damage_window(d, w);
    damage_focus(d);
}

struct window *desk_cycle(struct desktop *d)
{
    struct window *top = desk_focused(d);
    if (d->count < 2 || !top)
        return top;
    damage_focus(d);
    to_bottom(d, index_of(d, top));
    struct window *next = &d->windows[d->order[d->count - 1]];
    if (next->minimized)
        desk_restore(d, next);
    else
        desk_damage_window(d, next);
    damage_focus(d);
    return next;
}

struct rect desk_taskbar(const struct desktop *d)
{
    return rect_make(0, d->gfx->height - TASKBAR_H, d->gfx->width, TASKBAR_H);
}

struct rect desk_workarea(const struct desktop *d)
{
    return rect_make(0, TOPBAR_H, d->gfx->width, d->gfx->height - TOPBAR_H - TASKBAR_H);
}

void desk_damage_taskbar(struct desktop *d)
{
    gfx_damage(d->gfx, desk_taskbar(d));
}

static int task_width(const struct desktop *d, int n)
{
    int w = n ? (d->gfx->width - 6) / n - 6 : 0;
    return w > 200 ? 200 : w;
}

static struct rect task_button(const struct desktop *d, int slot, int n)
{
    struct rect bar = desk_taskbar(d);
    int w = task_width(d, n);
    return rect_make(6 + slot * (w + 6), bar.y + 4, w, TASKBAR_H - 7);
}

struct window *desk_taskbar_at(struct desktop *d, int x, int y)
{
    int n = 0;
    for (int i = 0; i < WIN_MAX; i++)
        if (d->windows[i].used)
            n++;
    int slot = 0;
    for (int i = 0; i < WIN_MAX; i++) {
        if (!d->windows[i].used)
            continue;
        if (rect_contains(task_button(d, slot, n), x, y))
            return &d->windows[i];
        slot++;
    }
    return NULL;
}

void desk_raise(struct desktop *d, struct window *w)
{
    int idx = index_of(d, w);
    if (d->count && d->order[d->count - 1] == idx)
        return;
    damage_focus(d);
    int out = 0;
    for (int i = 0; i < d->count; i++)
        if (d->order[i] != idx)
            d->order[out++] = d->order[i];
    d->order[out++] = idx;
    d->count = out;
    desk_damage_window(d, w);
}

void desk_move(struct desktop *d, struct window *w, int x, int y)
{
    if (x == w->frame.x && y == w->frame.y)
        return;
    desk_damage_window(d, w);
    w->frame.x = x;
    w->frame.y = y;
    desk_damage_window(d, w);
}

void desk_damage_window(struct desktop *d, const struct window *w)
{
    gfx_damage(d->gfx, w->frame);
}

void desk_damage_term_rows(struct desktop *d, struct window *w)
{
    for (int r = 0; r < w->term.rows; r++)
        if (term_take_row(&w->term, r)) {
            struct rect c = win_content(w);
            gfx_damage(d->gfx, rect_make(c.x, c.y + r * d->gfx->cell_h, c.w, d->gfx->cell_h));
        }
}

void desk_damage_topbar(struct desktop *d)
{
    gfx_damage(d->gfx, rect_make(0, 0, d->gfx->width, TOPBAR_H));
}

static void draw_desktop(struct desktop *d, struct rect area)
{
    struct gfx *g = d->gfx;
    int y0 = area.y;
    int y1 = area.y + area.h;
    if (d->wallpaper) {
        struct rect c = rect_clip(area, g->clip);
        for (int y = c.y; y < c.y + c.h; y++)
            memcpy(g->back + (size_t)y * (size_t)g->width + (size_t)c.x,
                   d->wallpaper + (size_t)y * (size_t)g->width + (size_t)c.x, (size_t)c.w * 4);
        return;
    }
    for (int y = y0; y < y1; y++) {
        int t = y * 255 / (g->height ? g->height : 1);
        uint32_t r = (((COL_DESK_TOP >> 16) & 0xFF) * (255 - t) + ((COL_DESK_BOT >> 16) & 0xFF) * t) / 255;
        uint32_t gg = (((COL_DESK_TOP >> 8) & 0xFF) * (255 - t) + ((COL_DESK_BOT >> 8) & 0xFF) * t) / 255;
        uint32_t b = ((COL_DESK_TOP & 0xFF) * (255 - t) + (COL_DESK_BOT & 0xFF) * t) / 255;
        gfx_fill(g, rect_make(area.x, y, area.w, 1), gfx_rgb(g, (r << 16) | (gg << 8) | b));
    }
}

static void draw_topbar(struct desktop *d)
{
    struct gfx *g = d->gfx;
    gfx_fill(g, rect_make(0, 0, g->width, TOPBAR_H), gfx_rgb(g, COL_TOPBAR));
    gfx_fill(g, rect_make(0, TOPBAR_H - 1, g->width, 1), gfx_rgb(g, COL_TOPBAR_ED));
    struct rect b = desk_menu_button(d);
    if (d->menu_open)
        gfx_fill(g, b, gfx_rgb(g, COL_MENU_HOVER));
    char stamp[80];
    snprintf(stamp, sizeof(stamp), "%s  %s  %s %s", d->ip, d->mem, d->date, d->clock);
    int len = (int)strlen(stamp);
    if (d->sans) {
        struct font *f = d->sans;
        int ty = (TOPBAR_H - f->line_h) / 2;
        font_text(g, f, 14, ty, "Orion", 5, d->menu_open ? COL_TITLE_TXT : COL_ACCENT);
        int sw = font_width(f, stamp, len);
        int sx = g->width - 14 - sw;
        font_text(g, f, sx, ty, stamp, len, COL_TITLE_TXT);
        int hx = b.x + b.w + 16;
        int n = font_fit(f, d->hint, sx - 24 - hx);
        font_text(g, f, hx, ty, d->hint, n, 0x7C8AA5);
        return;
    }
    gfx_text(g, 12, 6, "Orion", gfx_rgb(g, d->menu_open ? COL_TITLE_TXT : COL_ACCENT), GFX_ZOOM);
    int stamp_x = g->width - 12 - len * GFX_CELL_W;
    gfx_text(g, stamp_x, 6, stamp, gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);

    int hint_x = 12 + 12 * GFX_CELL_W;
    int room = (stamp_x - 16 - hint_x) / GFX_CELL_W;
    if (room > 0) {
        char hint[96];
        if (room > (int)sizeof(hint) - 1)
            room = (int)sizeof(hint) - 1;
        snprintf(hint, sizeof(hint), "%.*s", room, d->hint);
        gfx_text(g, hint_x, 6, hint, gfx_rgb(g, 0x7C8AA5), GFX_ZOOM);
    }
}

static void draw_taskbar(struct desktop *d)
{
    struct gfx *g = d->gfx;
    struct rect bar = desk_taskbar(d);
    gfx_fill(g, bar, gfx_rgb(g, COL_TOPBAR));
    gfx_fill(g, rect_make(0, bar.y, g->width, 1), gfx_rgb(g, COL_TOPBAR_ED));
    int n = 0;
    for (int i = 0; i < WIN_MAX; i++)
        if (d->windows[i].used)
            n++;
    struct window *focused = desk_focused(d);
    int slot = 0;
    for (int i = 0; i < WIN_MAX; i++) {
        struct window *w = &d->windows[i];
        if (!w->used)
            continue;
        struct rect b = task_button(d, slot++, n);
        uint32_t bg = w == focused ? COL_TITLE_ON : w->minimized ? COL_PANEL_BG : COL_TITLE_OFF;
        uint32_t fg = w->minimized ? 0x7C8AA5 : COL_TITLE_TXT;
        gfx_fill(g, b, gfx_rgb(g, bg));
        if (d->sans) {
            int len = font_fit(d->sans, w->title, b.w - 12);
            font_text(g, d->sans, b.x + 6, b.y + (b.h - d->sans->line_h) / 2, w->title, len, fg);
        } else {
            int room = (b.w - 8) / GFX_CELL_W;
            int len = (int)strlen(w->title);
            gfx_text_n(g, b.x + 4, b.y + 2, w->title, len < room ? len : (room > 0 ? room : 0), gfx_rgb(g, fg),
                       GFX_ZOOM);
        }
    }
}

static void draw_menu(struct desktop *d)
{
    struct gfx *g = d->gfx;
    struct rect m = desk_menu_rect(d);
    gfx_fill(g, m, gfx_rgb(g, COL_BORDER));
    gfx_fill(g, rect_make(m.x + 1, m.y + 1, m.w - 2, m.h - 2), gfx_rgb(g, COL_MENU_BG));
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = m.y + 4 + i * GFX_CELL_H;
        if (i == d->menu_hover)
            gfx_fill(g, rect_make(m.x + 2, y, m.w - 4, GFX_CELL_H), gfx_rgb(g, COL_MENU_HOVER));
        gfx_text(g, m.x + 10, y + 1, desk_menu_label(i), gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);
    }
}

static void draw_panel(struct desktop *d, struct window *w)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_PANEL_BG));
    for (int i = 0; i < w->nrows && i < PANEL_ROWS; i++) {
        int y = c.y + i * GFX_CELL_H;
        uint32_t fg = COL_TERM_FG;
        if (i == 0)
            fg = COL_ACCENT;
        else if (i == w->selected)
            gfx_fill(g, rect_make(c.x - 2, y, c.w + 4, GFX_CELL_H), gfx_rgb(g, COL_SELECT));
        gfx_text(g, c.x, y, w->rows_text[i], gfx_rgb(g, fg), GFX_ZOOM);
    }
}

static void draw_cell_char(struct desktop *d, int x, int y, char c, uint32_t rgb)
{
    if (c == ' ' || c == 0)
        return;
    if (d->mono)
        font_text(d->gfx, d->mono, x, y + 1, &c, 1, rgb);
    else
        gfx_char(d->gfx, x, y, c, gfx_rgb(d->gfx, rgb), GFX_ZOOM);
}

static void draw_terminal(struct desktop *d, struct window *w, bool focused)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_TERM_BG));

    int cw = g->cell_w, ch = g->cell_h;
    for (int r = 0; r < w->term.rows; r++) {
        int y = c.y + r * ch;
        if (y + ch <= g->clip.y || y >= g->clip.y + g->clip.h)
            continue;
        for (int col = 0; col < w->term.cols; col++) {
            const struct term_cell *cell = &w->term.cells[r][col];
            int x = c.x + col * cw;
            if (cell->bg != TERM_DEFAULT_BG)
                gfx_fill(g, rect_make(x, y, cw, ch), gfx_rgb(g, term_color(cell->bg)));
            draw_cell_char(d, x, y, cell->ch, term_color(cell->fg));
        }
    }

    if (!focused || !w->term.cursor_on)
        return;
    int cx = c.x + w->term.cur_col * cw;
    int cy = c.y + w->term.cur_row * ch;
    const struct term_cell *under = &w->term.cells[w->term.cur_row][w->term.cur_col];
    gfx_fill(g, rect_make(cx, cy, cw, ch), gfx_rgb(g, term_color(under->fg)));
    draw_cell_char(d, cx, cy, under->ch, term_color(under->bg));
}

static void draw_clock_window(struct desktop *d, struct window *w)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_PANEL_BG));
    gfx_text(g, c.x + 8, c.y + 6, d->clock, gfx_rgb(g, COL_ACCENT), 4);
    gfx_text(g, c.x + 8, c.y + 6 + GFX_FONT_H * 4 + 12, d->date, gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);
}

static void draw_about_window(struct desktop *d, struct window *w)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_PANEL_BG));
    for (size_t i = 0; i < sizeof(about_lines) / sizeof(about_lines[0]); i++)
        gfx_text(g, c.x + 4, c.y + 4 + (int)i * GFX_CELL_H, about_lines[i],
                 gfx_rgb(g, i == 0 ? COL_ACCENT : COL_TERM_FG), GFX_ZOOM);
}

static void draw_app_window(struct desktop *d, struct window *w)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_PANEL_BG));
    if (w->pixels)
        gfx_blit_rgb(g, c.x, c.y, w->pixels, w->pix_w, w->pix_h);
}

static void draw_window(struct desktop *d, struct window *w, bool focused)
{
    struct gfx *g = d->gfx;
    gfx_fill(g, w->frame, gfx_rgb(g, COL_BORDER));

    struct rect t = win_titlebar(w);
    gfx_fill(g, t, gfx_rgb(g, focused ? COL_TITLE_ON : COL_TITLE_OFF));
    struct rect b = win_close_button(w);
    struct rect mn = win_min_button(w);
    struct rect mx = win_max_button(w);
    int text_end = mx.w ? mx.x : mn.x;
    if (d->sans) {
        int n = font_fit(d->sans, w->title, text_end - t.x - 14);
        font_text(g, d->sans, t.x + 8, t.y + (t.h - d->sans->line_h) / 2, w->title, n, COL_TITLE_TXT);
    } else {
        int room = (text_end - t.x - 12) / GFX_CELL_W;
        int len = (int)strlen(w->title);
        gfx_text_n(g, t.x + 8, t.y + 3, w->title, len < room ? len : (room > 0 ? room : 0),
                   gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);
    }

    gfx_fill(g, rect_make(b.x + 4, b.y + 4, b.w - 8, b.h - 8),
             gfx_rgb(g, focused ? COL_CLOSE : COL_CLOSE_OFF));
    gfx_text(g, b.x + 5, b.y + 3, "x", gfx_rgb(g, 0x101418), GFX_ZOOM);
    uint32_t icon = gfx_rgb(g, focused ? COL_TITLE_TXT : 0x7C8AA5);
    gfx_fill(g, rect_make(mn.x + 6, mn.y + mn.h - 8, mn.w - 12, 2), icon);
    if (mx.w) {
        struct rect r = rect_make(mx.x + 6, mx.y + 6, mx.w - 12, mx.h - 12);
        gfx_fill(g, rect_make(r.x, r.y, r.w, 2), icon);
        gfx_fill(g, rect_make(r.x, r.y + r.h - 1, r.w, 1), icon);
        gfx_fill(g, rect_make(r.x, r.y, 1, r.h), icon);
        gfx_fill(g, rect_make(r.x + r.w - 1, r.y, 1, r.h), icon);
    }

    switch (w->kind) {
    case WIN_TERM:  draw_terminal(d, w, focused); break;
    case WIN_CLOCK: draw_clock_window(d, w); break;
    case WIN_ABOUT: draw_about_window(d, w); break;
    case WIN_PROCS:
    case WIN_FILES: draw_panel(d, w); break;
    case WIN_APP:   draw_app_window(d, w); break;
    }
}

void desk_compose(struct desktop *d, struct rect area)
{
    draw_desktop(d, area);
    if (rect_intersects(area, rect_make(0, 0, d->gfx->width, TOPBAR_H)))
        draw_topbar(d);
    struct window *focused = desk_focused(d);
    for (int i = 0; i < d->count; i++) {
        struct window *w = &d->windows[d->order[i]];
        if (!w->minimized && rect_intersects(area, w->frame))
            draw_window(d, w, w == focused);
    }
    if (rect_intersects(area, desk_taskbar(d)))
        draw_taskbar(d);
    if (d->menu_open && rect_intersects(area, desk_menu_rect(d)))
        draw_menu(d);
}
