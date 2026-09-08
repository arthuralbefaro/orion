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

static const char *const about_lines[] = {
    "Orion 1.0 - compositor em espaco de usuario",
    "",
    "Ctrl+T       novo terminal",
    "Ctrl+Q       fecha a janela focada",
    "Ctrl+Alt+X   encerra o wm",
    "",
    "Arraste pela barra de titulo para mover.",
    "Clique numa janela para focar e trazer a frente.",
};

void desk_init(struct desktop *d, struct gfx *g)
{
    memset(d, 0, sizeof(*d));
    d->gfx = g;
    d->hint = "Ctrl+T terminal  Ctrl+Q fecha  Ctrl+Alt+X sai";
    strcpy(d->clock, "--:--:--");
    strcpy(d->date, "--/--/----");
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
    if (!d->count)
        return NULL;
    return &d->windows[d->order[d->count - 1]];
}

struct window *desk_at(struct desktop *d, int x, int y)
{
    for (int i = d->count - 1; i >= 0; i--) {
        struct window *w = &d->windows[d->order[i]];
        if (rect_contains(w->frame, x, y))
            return w;
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
        if (term_take_row(&w->term, r))
            gfx_damage(d->gfx, win_row_rect(w, r));
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
    gfx_text(g, 12, 6, "Orion 1.0", gfx_rgb(g, COL_ACCENT), GFX_ZOOM);

    int hint_x = 12 + 10 * GFX_CELL_W;
    gfx_text(g, hint_x, 6, d->hint, gfx_rgb(g, 0x7C8AA5), GFX_ZOOM);

    char stamp[48];
    snprintf(stamp, sizeof(stamp), "%s %s", d->date, d->clock);
    int len = (int)strlen(stamp);
    gfx_text(g, g->width - 12 - len * GFX_CELL_W, 6, stamp, gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);
}

static void draw_terminal(struct desktop *d, struct window *w, bool focused)
{
    struct gfx *g = d->gfx;
    struct rect c = win_content(w);
    gfx_fill(g, rect_make(c.x - WIN_PAD, c.y - WIN_PAD, c.w + 2 * WIN_PAD, c.h + 2 * WIN_PAD),
             gfx_rgb(g, COL_TERM_BG));

    for (int r = 0; r < w->term.rows; r++) {
        int y = c.y + r * GFX_CELL_H;
        if (y + GFX_CELL_H <= g->clip.y || y >= g->clip.y + g->clip.h)
            continue;
        for (int col = 0; col < w->term.cols; col++) {
            const struct term_cell *cell = &w->term.cells[r][col];
            int x = c.x + col * GFX_CELL_W;
            if (cell->bg != TERM_DEFAULT_BG)
                gfx_fill(g, rect_make(x, y, GFX_CELL_W, GFX_CELL_H),
                         gfx_rgb(g, term_color(cell->bg)));
            gfx_char(g, x, y, cell->ch, gfx_rgb(g, term_color(cell->fg)), GFX_ZOOM);
        }
    }

    if (!focused || !w->term.cursor_on)
        return;
    int cx = c.x + w->term.cur_col * GFX_CELL_W;
    int cy = c.y + w->term.cur_row * GFX_CELL_H;
    const struct term_cell *under = &w->term.cells[w->term.cur_row][w->term.cur_col];
    gfx_fill(g, rect_make(cx, cy, GFX_CELL_W, GFX_FONT_H * GFX_ZOOM),
             gfx_rgb(g, term_color(under->fg)));
    gfx_char(g, cx, cy, under->ch, gfx_rgb(g, term_color(under->bg)), GFX_ZOOM);
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

static void draw_window(struct desktop *d, struct window *w, bool focused)
{
    struct gfx *g = d->gfx;
    gfx_fill(g, w->frame, gfx_rgb(g, COL_BORDER));

    struct rect t = win_titlebar(w);
    gfx_fill(g, t, gfx_rgb(g, focused ? COL_TITLE_ON : COL_TITLE_OFF));
    gfx_text(g, t.x + 8, t.y + 3, w->title, gfx_rgb(g, COL_TITLE_TXT), GFX_ZOOM);

    struct rect b = win_close_button(w);
    gfx_fill(g, rect_make(b.x + 4, b.y + 4, b.w - 8, b.h - 8),
             gfx_rgb(g, focused ? COL_CLOSE : COL_CLOSE_OFF));
    gfx_text(g, b.x + 5, b.y + 3, "x", gfx_rgb(g, 0x101418), GFX_ZOOM);

    switch (w->kind) {
    case WIN_TERM:  draw_terminal(d, w, focused); break;
    case WIN_CLOCK: draw_clock_window(d, w); break;
    case WIN_ABOUT: draw_about_window(d, w); break;
    }
}

void desk_compose(struct desktop *d, struct rect area)
{
    draw_desktop(d, area);
    if (rect_intersects(area, rect_make(0, 0, d->gfx->width, TOPBAR_H)))
        draw_topbar(d);
    for (int i = 0; i < d->count; i++) {
        struct window *w = &d->windows[d->order[i]];
        if (rect_intersects(area, w->frame))
            draw_window(d, w, i == d->count - 1);
    }
}
