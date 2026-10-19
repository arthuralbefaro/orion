#include "gui.h"
#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define COLS   48
#define ROWS   26
#define CELL_W 16
#define CELL_H 18
#define PAD    4

static struct gui g;
static struct gui_widget *view;
static struct term term;
static int master = -1;
static pid_t shell = -1;

static const char *extended_sequence(int code)
{
    switch (code) {
    case 0x48: return "\x1b[A";
    case 0x50: return "\x1b[B";
    case 0x4D: return "\x1b[C";
    case 0x4B: return "\x1b[D";
    case 0x47: return "\x1b[H";
    case 0x4F: return "\x1b[F";
    case 0x49: return "\x1b[5~";
    case 0x51: return "\x1b[6~";
    case 0x52: return "\x1b[2~";
    case 0x53: return "\x1b[3~";
    case 0x1C: return "\n";
    default:   return NULL;
    }
}

static bool spawn_shell(void)
{
    int slave;
    if (orion_openpty(&master, &slave) < 0) {
        fprintf(stderr, "terminal: openpty: %s\n", strerror(errno));
        return false;
    }
    shell = fork();
    if (shell < 0) {
        close(master);
        close(slave);
        return false;
    }
    if (shell == 0) {
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);
        signal(SIGPIPE, SIG_DFL);
        char cols[8], rows[8];
        snprintf(cols, sizeof(cols), "%d", COLS);
        snprintf(rows, sizeof(rows), "%d", ROWS);
        setenv("COLUMNS", cols, 1);
        setenv("LINES", rows, 1);
        char *args[] = { "sh", NULL };
        execv("/bin/sh", args);
        _exit(127);
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    struct winsize ws = { ROWS, COLS, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);
    return true;
}

static void finish(void)
{
    if (master >= 0)
        close(master);
    master = -1;
    g.watch_fd = -1;
    gui_quit(&g);
}

static void drain(void)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(master, buf, sizeof(buf));
        if (n > 0) {
            term_write(&term, buf, (int)n);
            view->version++;
            continue;
        }
        if (n == 0 || errno != EAGAIN)
            finish();
        return;
    }
}

static void on_watch(struct gui *gu)
{
    (void)gu;
    drain();
}

static void on_tick(struct gui *gu)
{
    (void)gu;
    if (shell > 0 && waitpid(shell, NULL, WNOHANG) == shell) {
        shell = -1;
        if (master >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(master, buf, sizeof(buf))) > 0)
                term_write(&term, buf, (int)n);
        }
        finish();
    }
}

static void paint(struct gui *gu, struct gui_widget *w)
{
    gui_fill(gu, w->x, w->y, w->w, w->h, term_color(TERM_DEFAULT_BG));
    char glyph[2] = { 0, 0 };
    for (int r = 0; r < term.rows; r++) {
        for (int c = 0; c < term.cols; c++) {
            struct term_cell *cell = &term.cells[r][c];
            bool cursor = term.cursor_on && r == term.cur_row && c == term.cur_col;
            uint32_t fg = term_color(cursor ? cell->bg : cell->fg);
            uint32_t bg = term_color(cursor ? cell->fg : cell->bg);
            int x = w->x + PAD + c * CELL_W, y = w->y + PAD + r * CELL_H;
            if (bg != term_color(TERM_DEFAULT_BG))
                gui_fill(gu, x, y, CELL_W, CELL_H, bg);
            if (cell->ch != ' ' && cell->ch) {
                glyph[0] = cell->ch;
                gui_draw_text(gu, x, y + 1, glyph, fg);
            }
        }
    }
}

static bool on_key(struct gui *gu, int ascii, int scancode, int mods)
{
    (void)gu;
    if (master < 0)
        return true;
    if (mods & GUI_MOD_EXTENDED) {
        const char *seq = extended_sequence(scancode);
        if (seq)
            write(master, seq, strlen(seq));
        return true;
    }
    if (ascii) {
        char c = (char)ascii;
        write(master, &c, 1);
    }
    return true;
}

int main(void)
{
    if (!gui_open(&g, COLS * CELL_W + 2 * PAD + 20, ROWS * CELL_H + 2 * PAD + 20, "terminal"))
        return 1;
    term_init(&term, COLS, ROWS);
    view = gui_canvas(&g, 10, 10, COLS * CELL_W + 2 * PAD, ROWS * CELL_H + 2 * PAD, paint, NULL);
    if (!spawn_shell()) {
        gui_close(&g);
        return 1;
    }
    g.on_key = on_key;
    g.on_watch = on_watch;
    g.watch_fd = master;
    g.on_tick = on_tick;
    g.tick_ms = 500;
    gui_set_title(&g, "terminal: sh pid %d", (int)shell);
    gui_run(&g);
    if (master >= 0)
        close(master);
    if (shell > 0) {
        kill(shell, SIGKILL);
        waitpid(shell, NULL, 0);
    }
    return 0;
}
