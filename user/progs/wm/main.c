#include "gfx.h"
#include "term.h"
#include "win.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/input.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TERM_COLS      60
#define TERM_ROWS      20
#define POLL_TIMEOUT   250
#define KEY_CTRL_T     0x14
#define KEY_CTRL_Q     0x11
#define KEY_CTRL_X     0x18

static struct gfx gfx;
static struct desktop desk;
static int fb_fd = -1;
static int input_fd = -1;
static bool running = true;
static int mouse_x, mouse_y;
static uint32_t buttons;
static struct window *drag_win;
static int drag_dx, drag_dy;
static long last_clock;
static int spawned;

static void compose_cb(struct rect area)
{
    desk_compose(&desk, area);
}

static struct rect term_frame(int cols, int rows)
{
    return rect_make(0, 0, cols * GFX_CELL_W + WIN_CHROME_X, rows * GFX_CELL_H + WIN_CHROME_Y);
}

static void close_extra_fds(int keep)
{
    for (int i = 0; i < WIN_MAX; i++) {
        struct window *w = &desk.windows[i];
        if (w->used && w->kind == WIN_TERM && w->master >= 0 && w->master != keep)
            close(w->master);
    }
    close(fb_fd);
    close(input_fd);
}

static bool spawn_shell(struct window *w)
{
    int master, slave;
    if (openpty(&master, &slave) < 0) {
        fprintf(stderr, "wm: openpty: %s\n", strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "wm: fork: %s\n", strerror(errno));
        close(master);
        close(slave);
        return false;
    }
    if (pid == 0) {
        close_extra_fds(master);
        close(master);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);
        char *args[] = { "sh", NULL };
        execv("/bin/sh", args);
        _exit(127);
    }
    close(slave);
    fcntl(master, F_SETFL, O_NONBLOCK);
    struct winsize ws = { TERM_ROWS, TERM_COLS, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);
    w->master = master;
    w->pid = pid;
    fprintf(stderr, "wm: terminal %d -> sh pid %d (master fd %d)\n", w->id, pid, master);
    return true;
}

static struct window *new_terminal(void)
{
    char title[32];
    int step = spawned % 6;
    struct rect f = term_frame(TERM_COLS, TERM_ROWS);
    f.x = 40 + step * 28;
    f.y = 56 + step * 26;
    snprintf(title, sizeof(title), "Terminal %d", spawned + 1);

    struct window *w = desk_new(&desk, WIN_TERM, title, f);
    if (!w)
        return NULL;
    term_init(&w->term, TERM_COLS, TERM_ROWS);
    if (!spawn_shell(w)) {
        desk_remove(&desk, w);
        return NULL;
    }
    spawned++;
    desk_damage_window(&desk, w);
    return w;
}

static void close_window(struct window *w)
{
    desk_damage_window(&desk, w);
    if (w->kind == WIN_TERM && w->master >= 0) {
        fprintf(stderr, "wm: fechando terminal %d (sh pid %d)\n", w->id, w->pid);
        close(w->master);
    }
    desk_remove(&desk, w);
    struct window *top = desk_focused(&desk);
    if (top)
        desk_damage_window(&desk, top);
}

static void reap_children(void)
{
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}

static void update_clock(void)
{
    time_t now = time(NULL);
    if (now == last_clock)
        return;
    last_clock = now;

    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(desk.clock, sizeof(desk.clock), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(desk.date, sizeof(desk.date), "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1,
             tm.tm_year + 1900);

    desk_damage_topbar(&desk);
    for (int i = 0; i < desk.count; i++) {
        struct window *w = &desk.windows[desk.order[i]];
        if (w->kind == WIN_CLOCK)
            desk_damage_window(&desk, w);
    }
}

static void pump_terminals(void)
{
    char buf[512];
    for (int i = desk.count - 1; i >= 0; i--) {
        struct window *w = &desk.windows[desk.order[i]];
        if (w->kind != WIN_TERM || w->master < 0)
            continue;
        for (int round = 0; round < 8; round++) {
            long n = read(w->master, buf, sizeof(buf));
            if (n > 0) {
                term_write(&w->term, buf, (int)n);
                continue;
            }
            if (n == 0) {
                fprintf(stderr, "wm: terminal %d encerrado\n", w->id);
                close_window(w);
            }
            break;
        }
        if (w->used)
            desk_damage_term_rows(&desk, w);
    }
}

static void handle_button(int32_t mask)
{
    bool was_down = (buttons & INPUT_BUTTON_LEFT) != 0;
    bool is_down = (mask & INPUT_BUTTON_LEFT) != 0;
    buttons = (uint32_t)mask;

    if (was_down && !is_down) {
        drag_win = NULL;
        return;
    }
    if (!is_down || was_down)
        return;

    struct window *w = desk_at(&desk, mouse_x, mouse_y);
    if (!w)
        return;
    desk_raise(&desk, w);

    if (rect_contains(win_close_button(w), mouse_x, mouse_y)) {
        close_window(w);
        return;
    }
    if (rect_contains(win_titlebar(w), mouse_x, mouse_y)) {
        drag_win = w;
        drag_dx = mouse_x - w->frame.x;
        drag_dy = mouse_y - w->frame.y;
    }
}

static void handle_motion(int32_t dx, int32_t dy)
{
    gfx_damage(&gfx, gfx_cursor_rect(&gfx));
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx.width) mouse_x = gfx.width - 1;
    if (mouse_y >= gfx.height) mouse_y = gfx.height - 1;
    gfx_cursor_move(&gfx, mouse_x, mouse_y);
    gfx_damage(&gfx, gfx_cursor_rect(&gfx));

    if (!drag_win)
        return;
    int nx = mouse_x - drag_dx;
    int ny = mouse_y - drag_dy;
    if (ny < TOPBAR_H)
        ny = TOPBAR_H;
    if (nx < -drag_win->frame.w + 80)
        nx = -drag_win->frame.w + 80;
    if (nx > gfx.width - 80)
        nx = gfx.width - 80;
    if (ny > gfx.height - WIN_TITLE_H)
        ny = gfx.height - WIN_TITLE_H;
    desk_move(&desk, drag_win, nx, ny);
}

static const char *extended_sequence(int32_t code)
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

static void handle_key(int32_t scancode, int32_t ascii, int32_t mods)
{
    (void)scancode;
    if ((mods & INPUT_MOD_CTRL) && (mods & INPUT_MOD_ALT) && ascii == KEY_CTRL_X) {
        running = false;
        return;
    }
    if (mods & INPUT_MOD_CTRL) {
        if (ascii == KEY_CTRL_T) {
            new_terminal();
            return;
        }
        if (ascii == KEY_CTRL_Q) {
            struct window *w = desk_focused(&desk);
            if (w)
                close_window(w);
            return;
        }
    }
    struct window *w = desk_focused(&desk);
    if (!w || w->kind != WIN_TERM || w->master < 0)
        return;

    if (mods & INPUT_MOD_EXTENDED) {
        const char *seq = extended_sequence(scancode);
        if (seq)
            write(w->master, seq, strlen(seq));
        return;
    }
    if (!ascii)
        return;
    char c = (char)ascii;
    write(w->master, &c, 1);
}

static void handle_event(const struct input_event *ev)
{
    switch (ev->type) {
    case INPUT_EV_KEY_DOWN:   handle_key(ev->a, ev->b, ev->c); break;
    case INPUT_EV_MOUSE_MOVE: handle_motion(ev->a, ev->b); break;
    case INPUT_EV_MOUSE_BUTTON: handle_button(ev->a); break;
    default: break;
    }
}

static void build_desktop(void)
{
    desk_init(&desk, &gfx);
    new_terminal();

    struct rect clock_frame = rect_make(40, 486, 8 * GFX_FONT_W * 4 + 24 + WIN_CHROME_X,
                                        GFX_FONT_H * 4 + GFX_CELL_H + 28 + WIN_CHROME_Y);
    desk_new(&desk, WIN_CLOCK, "Relogio", clock_frame);

    struct rect about_frame = rect_make(clock_frame.x + clock_frame.w + 24, 486,
                                        50 * GFX_CELL_W + 8 + WIN_CHROME_X,
                                        8 * GFX_CELL_H + 8 + WIN_CHROME_Y);
    desk_new(&desk, WIN_ABOUT, "Sobre", about_frame);

    struct window *term = &desk.windows[0];
    if (term->used)
        desk_raise(&desk, term);
}

static void shutdown_desktop(void)
{
    for (int i = 0; i < WIN_MAX; i++) {
        struct window *w = &desk.windows[i];
        if (w->used && w->kind == WIN_TERM && w->master >= 0) {
            close(w->master);
            w->master = -1;
        }
    }
    for (int i = 0; i < 60; i++) {
        reap_children();
        usleep(50000);
    }
    fprintf(stderr, "wm: %lu quadro(s), %lu pixel(s) copiado(s) para o framebuffer\n",
            gfx.frames, gfx.flushed_pixels);
    gfx_close(&gfx);
    ioctl(fb_fd, FBIOREDRAW, NULL);
    close(fb_fd);
    close(input_fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "wm: /dev/fb0: %s\n", strerror(errno));
        return 1;
    }
    input_fd = open("/dev/input", O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "wm: /dev/input: %s\n", strerror(errno));
        return 1;
    }
    if (!gfx_open(&gfx, fb_fd))
        return 1;
    fprintf(stderr, "wm: %dx%d, celula %dx%d\n", gfx.width, gfx.height, GFX_CELL_W, GFX_CELL_H);

    mouse_x = gfx.width / 2;
    mouse_y = gfx.height / 2;
    gfx_cursor_move(&gfx, mouse_x, mouse_y);

    build_desktop();
    update_clock();
    gfx_damage(&gfx, rect_make(0, 0, gfx.width, gfx.height));

    struct input_event events[32];
    while (running) {
        struct pollfd fds[WIN_MAX + 1];
        int n = 0;
        fds[n].fd = input_fd;
        fds[n].events = POLLIN;
        n++;
        for (int i = 0; i < desk.count && n < WIN_MAX + 1; i++) {
            struct window *w = &desk.windows[desk.order[i]];
            if (w->kind == WIN_TERM && w->master >= 0) {
                fds[n].fd = w->master;
                fds[n].events = POLLIN;
                n++;
            }
        }
        poll(fds, (nfds_t)n, POLL_TIMEOUT);

        if (fds[0].revents & POLLIN) {
            long got = read(input_fd, events, sizeof(events));
            for (long i = 0; i < got / (long)sizeof(events[0]); i++)
                handle_event(&events[i]);
        }
        pump_terminals();
        update_clock();
        reap_children();
        if (gfx_has_damage(&gfx))
            gfx_flush(&gfx, compose_cb);
    }

    fprintf(stderr, "wm: encerrando\n");
    shutdown_desktop();
    return 0;
}
