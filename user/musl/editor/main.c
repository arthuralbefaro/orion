#include "gui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CELL_W     16
#define CELL_H     18
#define PAD        4
#define COL_PAGE   0x141A26
#define COL_TEXT   0xE8ECF4
#define COL_CURSOR 0xFFD060
#define KEY_CTRL_S 19
#define SC_HOME    0x47
#define SC_END     0x4F
#define SC_DELETE  0x53

struct line {
    char *s;
    int len, cap;
};

static struct gui g;
static struct gui_widget *view, *status;
static struct line *lines;
static int nlines, cap_lines;
static int cur_line, cur_col, top_line, left_col;
static bool modified;
static char path[256];
static const char *name;

static void reserve(struct line *l, int need)
{
    if (need + 1 <= l->cap)
        return;
    int cap = l->cap ? l->cap : 16;
    while (cap < need + 1)
        cap *= 2;
    char *s = realloc(l->s, (size_t)cap);
    if (!s)
        exit(1);
    l->s = s;
    l->cap = cap;
}

static struct line *insert_line(int at)
{
    if (nlines == cap_lines) {
        int cap = cap_lines ? cap_lines * 2 : 64;
        struct line *n = realloc(lines, sizeof(lines[0]) * (size_t)cap);
        if (!n)
            exit(1);
        lines = n;
        cap_lines = cap;
    }
    memmove(&lines[at + 1], &lines[at], sizeof(lines[0]) * (size_t)(nlines - at));
    nlines++;
    lines[at] = (struct line){ 0 };
    reserve(&lines[at], 0);
    lines[at].s[0] = 0;
    return &lines[at];
}

static void remove_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], sizeof(lines[0]) * (size_t)(nlines - at - 1));
    nlines--;
}

static void append(struct line *l, const char *s, int n)
{
    reserve(l, l->len + n);
    memcpy(l->s + l->len, s, (size_t)n);
    l->len += n;
    l->s[l->len] = 0;
}

static int rows(void)
{
    return (view->h - 2 * PAD) / CELL_H;
}

static int cols(void)
{
    return (view->w - 2 * PAD) / CELL_W;
}

static void update_status(void)
{
    gui_set_text(status, "%s  lin %d col %d%s", name, cur_line + 1, cur_col + 1, modified ? "  *" : "");
}

static void set_modified(bool m)
{
    if (m == modified)
        return;
    modified = m;
    gui_set_title(&g, "editor: %s%s", name, m ? " *" : "");
}

static void changed(void)
{
    if (cur_line < top_line)
        top_line = cur_line;
    if (cur_line >= top_line + rows())
        top_line = cur_line - rows() + 1;
    if (cur_col < left_col)
        left_col = cur_col;
    if (cur_col >= left_col + cols())
        left_col = cur_col - cols() + 1;
    view->version++;
    update_status();
}

static void load(void)
{
    insert_line(0);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        gui_set_title(&g, "editor: %s", name);
        return;
    }
    char buf[4096];
    ssize_t n;
    struct line *l = &lines[0];
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n')
                l = insert_line(nlines);
            else if (buf[i] != '\r')
                append(l, &buf[i], 1);
        }
    }
    close(fd);
    if (nlines > 1 && lines[nlines - 1].len == 0)
        remove_line(nlines - 1);
    gui_set_title(&g, "editor: %s", name);
}

static bool save(void)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        gui_set_text(status, "%s: %s", name, strerror(errno));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < nlines && ok; i++) {
        if (lines[i].len && write(fd, lines[i].s, (size_t)lines[i].len) != lines[i].len)
            ok = false;
        if (ok && write(fd, "\n", 1) != 1)
            ok = false;
    }
    if (close(fd) < 0)
        ok = false;
    if (!ok) {
        gui_set_text(status, "%s: erro ao gravar", name);
        return false;
    }
    modified = false;
    gui_set_title(&g, "editor: salvou %s", name);
    update_status();
    return true;
}

static void type_char(char c)
{
    struct line *l = &lines[cur_line];
    reserve(l, l->len + 1);
    memmove(l->s + cur_col + 1, l->s + cur_col, (size_t)(l->len - cur_col + 1));
    l->s[cur_col++] = c;
    l->len++;
    set_modified(true);
}

static void new_line(void)
{
    struct line *n = insert_line(cur_line + 1);
    struct line *l = &lines[cur_line];
    append(n, l->s + cur_col, l->len - cur_col);
    l->len = cur_col;
    l->s[l->len] = 0;
    cur_line++;
    cur_col = 0;
    set_modified(true);
}

static void backspace(void)
{
    if (cur_col > 0) {
        struct line *l = &lines[cur_line];
        memmove(l->s + cur_col - 1, l->s + cur_col, (size_t)(l->len - cur_col + 1));
        l->len--;
        cur_col--;
    } else if (cur_line > 0) {
        struct line *prev = &lines[cur_line - 1];
        cur_col = prev->len;
        append(prev, lines[cur_line].s, lines[cur_line].len);
        remove_line(cur_line);
        cur_line--;
    } else {
        return;
    }
    set_modified(true);
}

static void delete_char(void)
{
    struct line *l = &lines[cur_line];
    if (cur_col < l->len) {
        memmove(l->s + cur_col, l->s + cur_col + 1, (size_t)(l->len - cur_col));
        l->len--;
    } else if (cur_line + 1 < nlines) {
        append(l, lines[cur_line + 1].s, lines[cur_line + 1].len);
        remove_line(cur_line + 1);
    } else {
        return;
    }
    set_modified(true);
}

static void move_to(int line, int col)
{
    if (line < 0)
        line = 0;
    if (line >= nlines)
        line = nlines - 1;
    if (col < 0)
        col = 0;
    if (col > lines[line].len)
        col = lines[line].len;
    cur_line = line;
    cur_col = col;
}

static void paint(struct gui *gu, struct gui_widget *w)
{
    gui_fill(gu, w->x, w->y, w->w, w->h, COL_PAGE);
    int nr = rows(), nc = cols();
    char buf[128];
    for (int r = 0; r < nr && top_line + r < nlines; r++) {
        struct line *l = &lines[top_line + r];
        int n = l->len - left_col;
        if (n <= 0)
            continue;
        if (n > nc)
            n = nc;
        if (n > (int)sizeof(buf) - 1)
            n = (int)sizeof(buf) - 1;
        memcpy(buf, l->s + left_col, (size_t)n);
        buf[n] = 0;
        gui_draw_text(gu, w->x + PAD, w->y + PAD + r * CELL_H + 1, buf, COL_TEXT);
    }
    int cx = w->x + PAD + (cur_col - left_col) * CELL_W;
    int cy = w->y + PAD + (cur_line - top_line) * CELL_H;
    gui_fill(gu, cx, cy, 2, CELL_H, COL_CURSOR);
}

static bool on_key(struct gui *gu, int ascii, int scancode, int mods)
{
    (void)gu;
    if (mods & GUI_MOD_EXTENDED) {
        int page = rows() - 1;
        switch (scancode) {
        case GUI_SC_UP:    move_to(cur_line - 1, cur_col); break;
        case GUI_SC_DOWN:  move_to(cur_line + 1, cur_col); break;
        case GUI_SC_LEFT:
            if (cur_col > 0)
                cur_col--;
            else if (cur_line > 0)
                move_to(cur_line - 1, lines[cur_line - 1].len);
            break;
        case GUI_SC_RIGHT:
            if (cur_col < lines[cur_line].len)
                cur_col++;
            else if (cur_line + 1 < nlines)
                move_to(cur_line + 1, 0);
            break;
        case SC_HOME:      cur_col = 0; break;
        case SC_END:       cur_col = lines[cur_line].len; break;
        case GUI_SC_PGUP:  move_to(cur_line - page, cur_col); break;
        case GUI_SC_PGDN:  move_to(cur_line + page, cur_col); break;
        case SC_DELETE:    delete_char(); break;
        default:           return true;
        }
    } else if (ascii == KEY_CTRL_S) {
        save();
    } else if (ascii == GUI_KEY_ENTER || ascii == '\r') {
        new_line();
    } else if (ascii == GUI_KEY_BACKSPACE || ascii == 127) {
        backspace();
    } else if (ascii == GUI_KEY_TAB) {
        for (int i = 0; i < 4; i++)
            type_char(' ');
    } else if (ascii >= 32 && ascii < 127) {
        type_char((char)ascii);
    } else {
        return true;
    }
    changed();
    return true;
}

static void on_click(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    move_to(top_line + (gu->click_y - PAD) / CELL_H, left_col + (gu->click_x - PAD + CELL_W / 2) / CELL_W);
    changed();
}

static void on_save(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    save();
}

static void on_close(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    gui_quit(gu);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "uso: editor <arquivo>\n");
        return 2;
    }
    snprintf(path, sizeof(path), "%s", argv[1]);
    const char *slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
    if (!gui_open(&g, 640, 470, "editor"))
        return 1;
    g.on_key = on_key;
    view = gui_canvas(&g, 10, 10, 620, 386, paint, on_click);
    gui_button(&g, 10, 404, 200, "Salvar", on_save);
    gui_button(&g, 430, 404, 200, "Fechar", on_close);
    status = gui_label(&g, 10, 440, 620, "");
    load();
    update_status();
    gui_run(&g);
    return 0;
}
