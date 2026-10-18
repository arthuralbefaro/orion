#include "gfx.h"
#include "term.h"
#include "win.h"
#include "app.h"

#include <dirent.h>
#include <orion.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define KEY_CTRL_P     0x10
#define KEY_CTRL_F     0x06
#define KEY_CTRL_M     0x0D
#define KEY_CTRL_X     0x18
#define CTL_PATH       "/run/wm.sock"
#define CTL_MAX        8

static struct gfx gfx;
static struct desktop desk;
static int ctl_fd = -1;
static int ctl_clients[CTL_MAX];
static int ctl_count;
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
    if (orion_openpty(&master, &slave) < 0) {
        fprintf(stderr, "wm: openpty: %s\n", orion_strerror(errno));
        return false;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "wm: fork: %s\n", orion_strerror(errno));
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
    if (w->kind == WIN_APP) {
        app_close_window(w);
        return;
    }
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

static void refresh_procs(struct window *w)
{
    struct procinfo_orion list[PROCLIST_MAX];
    int n = orion_proclist(list, PROCLIST_MAX);
    if (n < 0)
        n = 0;

    struct sysinfo_orion si;
    char head[PANEL_COLS];
    if (orion_sysinfo(&si) == 0)
        snprintf(head, sizeof(head), "%4s %5s %8s  %s   (%u CPUs)", "pid", "ppid", "paginas",
                 "nome", si.cpus);
    else
        snprintf(head, sizeof(head), "%4s %5s %8s  %s", "pid", "ppid", "paginas", "nome");
    panel_set(w, 0, head);

    int row = 1;
    for (int i = 0; i < n && row < PANEL_ROWS; i++) {
        char line[PANEL_COLS];
        snprintf(line, sizeof(line), "%4u %5u %8lu  %s", list[i].pid, list[i].ppid,
                 list[i].pages, list[i].name);
        panel_set(w, row++, line);
    }
    w->nrows = row;
    desk_damage_window(&desk, w);
}

static void refresh_files(struct window *w)
{
    char head[PANEL_COLS];
    snprintf(head, sizeof(head), "%s", w->path);
    panel_set(w, 0, head);

    int row = 1;
    DIR *d = opendir(w->path);
    if (!d) {
        panel_set(w, row++, "(nao consegui abrir)");
        w->nrows = row;
        desk_damage_window(&desk, w);
        return;
    }
    if (strcmp(w->path, "/") != 0)
        panel_set(w, row++, "..");
    struct dirent *e;
    while ((e = readdir(d)) && row < PANEL_ROWS) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[256];
        snprintf(full, sizeof(full), "%s%s%s", w->path,
                 strcmp(w->path, "/") == 0 ? "" : "/", e->d_name);
        struct stat st;
        char line[PANEL_COLS];
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(line, sizeof(line), "%-32s <dir>", e->d_name);
        else if (stat(full, &st) == 0)
            snprintf(line, sizeof(line), "%-32s %8lu", e->d_name, (unsigned long)st.st_size);
        else
            snprintf(line, sizeof(line), "%s", e->d_name);
        panel_set(w, row++, line);
    }
    closedir(d);
    w->nrows = row;
    desk_damage_window(&desk, w);
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

    struct sysinfo_orion si;
    if (orion_sysinfo(&si) == 0)
        snprintf(desk.mem, sizeof(desk.mem), "%lu/%lu MiB", si.used_pages * 4096 / (1024 * 1024),
                 si.total_pages * 4096 / (1024 * 1024));

    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 0) {
        struct ifreq_orion r;
        memset(&r, 0, sizeof(r));
        if (ioctl(sfd, SIOCGIFCONF_ORION, &r) == 0 && r.ip)
            orion_inet_ntoa(r.ip, desk.ip, sizeof(desk.ip));
        else
            snprintf(desk.ip, sizeof(desk.ip), "sem rede");
        close(sfd);
    }

    desk_damage_topbar(&desk);
    for (int i = 0; i < desk.count; i++) {
        struct window *w = &desk.windows[desk.order[i]];
        if (w->kind == WIN_CLOCK)
            desk_damage_window(&desk, w);
        else if (w->kind == WIN_PROCS)
            refresh_procs(w);
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

static void open_procs(void)
{
    struct rect f = rect_make(120, 120, 58 * GFX_CELL_W + 8 + WIN_CHROME_X,
                              PANEL_ROWS * GFX_CELL_H + 8 + WIN_CHROME_Y);
    struct window *w = desk_new(&desk, WIN_PROCS, "Processos", f);
    if (!w)
        return;
    w->selected = -1;
    refresh_procs(w);
    desk_raise(&desk, w);
    printf("wm: janela de processos aberta\n");
}

static void open_files(const char *path)
{
    struct rect f = rect_make(200, 160, 46 * GFX_CELL_W + 8 + WIN_CHROME_X,
                              PANEL_ROWS * GFX_CELL_H + 8 + WIN_CHROME_Y);
    struct window *w = desk_new(&desk, WIN_FILES, "Arquivos", f);
    if (!w)
        return;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->selected = -1;
    refresh_files(w);
    desk_raise(&desk, w);
    printf("wm: janela de arquivos em %s\n", w->path);
}

static void menu_activate(int item)
{
    switch (item) {
    case 0: new_terminal(); break;
    case 1: open_procs(); break;
    case 2: open_files("/"); break;
    case 3: {
        struct rect f = rect_make(360, 300, 50 * GFX_CELL_W + 8 + WIN_CHROME_X,
                                  8 * GFX_CELL_H + 8 + WIN_CHROME_Y);
        struct window *w = desk_new(&desk, WIN_ABOUT, "Sobre", f);
        if (w)
            desk_raise(&desk, w);
        break;
    }
    case 4: running = false; break;
    default: break;
    }
}

static void files_click(struct window *w, int row)
{
    if (row < 1 || row >= w->nrows)
        return;
    w->selected = row;
    const char *name = w->rows_text[row];
    char item[128];
    int k = 0;
    while (name[k] && name[k] != ' ' && k < (int)sizeof(item) - 1) {
        item[k] = name[k];
        k++;
    }
    item[k] = '\0';

    char next[128];
    if (strcmp(item, "..") == 0) {
        snprintf(next, sizeof(next), "%s", w->path);
        char *slash = strrchr(next, '/');
        if (slash && slash != next)
            *slash = '\0';
        else
            snprintf(next, sizeof(next), "/");
    } else {
        snprintf(next, sizeof(next), "%s%s%s", w->path,
                 strcmp(w->path, "/") == 0 ? "" : "/", item);
    }
    struct stat st;
    if (stat(next, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(w->path, sizeof(w->path), "%s", next);
        w->selected = -1;
    }
    refresh_files(w);
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

    if (desk.menu_open) {
        int item = desk_menu_item_at(&desk, mouse_x, mouse_y);
        desk.menu_open = false;
        desk_damage_menu(&desk);
        desk_damage_topbar(&desk);
        if (item >= 0) {
            menu_activate(item);
            return;
        }
    }
    if (rect_contains(desk_menu_button(&desk), mouse_x, mouse_y)) {
        desk.menu_open = true;
        desk.menu_hover = -1;
        desk_damage_menu(&desk);
        desk_damage_topbar(&desk);
        return;
    }
    if (mouse_y < TOPBAR_H)
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
        return;
    }
    if (w->kind == WIN_APP) {
        struct rect c = win_content(w);
        if (rect_contains(c, mouse_x, mouse_y))
            app_click(w, mouse_x - c.x, mouse_y - c.y, (uint32_t)mask);
        return;
    }
    if (w->kind == WIN_FILES || w->kind == WIN_PROCS) {
        struct rect c = win_content(w);
        int row = (mouse_y - c.y) / GFX_CELL_H;
        if (w->kind == WIN_FILES)
            files_click(w, row);
        else if (row >= 1 && row < w->nrows) {
            w->selected = row;
            desk_damage_window(&desk, w);
        }
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

    if (desk.menu_open) {
        int hover = desk_menu_item_at(&desk, mouse_x, mouse_y);
        if (hover != desk.menu_hover) {
            desk.menu_hover = hover;
            desk_damage_menu(&desk);
        }
    }
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
        if (ascii == KEY_CTRL_P) {
            open_procs();
            return;
        }
        if (ascii == KEY_CTRL_F) {
            open_files("/");
            return;
        }
    }
    struct window *w = desk_focused(&desk);
    if (w && w->kind == WIN_APP) {
        app_key(w, ascii, scancode, mods);
        return;
    }
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

static void ctl_open(void)
{
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (ctl_fd < 0) {
        fprintf(stderr, "wm: socket de comandos: %s\n", orion_strerror(errno));
        return;
    }
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", CTL_PATH);
    unlink(CTL_PATH);
    if (bind(ctl_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ctl_fd, 4) < 0) {
        fprintf(stderr, "wm: %s: %s\n", CTL_PATH, orion_strerror(errno));
        close(ctl_fd);
        ctl_fd = -1;
        return;
    }
    fprintf(stderr, "wm: comandos em %s\n", CTL_PATH);
}

static void ctl_drop(int i)
{
    close(ctl_clients[i]);
    ctl_clients[i] = ctl_clients[--ctl_count];
}

static void ctl_accept(void)
{
    for (;;) {
        int c = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c < 0)
            return;
        if (ctl_count == CTL_MAX) {
            close(c);
            continue;
        }
        ctl_clients[ctl_count++] = c;
    }
}

static void ctl_reply(int fd, const char *text)
{
    size_t n = strlen(text);
    while (n) {
        ssize_t w = write(fd, text, n);
        if (w <= 0)
            return;
        text += w;
        n -= (size_t)w;
    }
}

static void ctl_command(int fd, char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    fprintf(stderr, "wm: wmctl: %s\n", line);
    char out[1024];
    if (strcmp(line, "term") == 0) {
        struct window *w = new_terminal();
        snprintf(out, sizeof(out), w ? "ok: terminal %d\n" : "erro: terminal nao abriu\n", w ? w->id : 0);
        ctl_reply(fd, out);
    } else if (strcmp(line, "procs") == 0) {
        open_procs();
        ctl_reply(fd, "ok: processos\n");
    } else if (strncmp(line, "files ", 6) == 0 && line[6] == '/') {
        open_files(line + 6);
        ctl_reply(fd, "ok: arquivos\n");
    } else if (strcmp(line, "list") == 0) {
        size_t used = 0;
        for (int i = 0; i < desk.count && used < sizeof(out) - 64; i++) {
            struct window *w = &desk.windows[desk.order[i]];
            used += (size_t)snprintf(out + used, sizeof(out) - used, "janela %d: %s\n", w->id, w->title);
        }
        out[used] = 0;
        ctl_reply(fd, out);
    } else if (strcmp(line, "ping") == 0) {
        ctl_reply(fd, "pong\n");
    } else {
        ctl_reply(fd, "erro: comandos: term, procs, files <caminho>, list, ping\n");
    }
    gfx_damage(&gfx, rect_make(0, 0, gfx.width, gfx.height));
}

static void ctl_serve(int i)
{
    char line[256];
    ssize_t n = read(ctl_clients[i], line, sizeof(line) - 1);
    if (n < 0 && errno == EAGAIN)
        return;
    if (n > 0) {
        line[n] = 0;
        ctl_command(ctl_clients[i], line);
    }
    ctl_drop(i);
}

static void shutdown_desktop(void)
{
    app_shutdown();
    while (ctl_count)
        ctl_drop(0);
    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink(CTL_PATH);
    }
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

    setvbuf(stdout, NULL, _IONBF, 0);

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "wm: /dev/fb0: %s\n", orion_strerror(errno));
        return 1;
    }
    input_fd = open("/dev/input", O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "wm: /dev/input: %s\n", orion_strerror(errno));
        return 1;
    }
    if (!gfx_open(&gfx, fb_fd))
        return 1;
    fprintf(stderr, "wm: %dx%d, celula %dx%d\n", gfx.width, gfx.height, GFX_CELL_W, GFX_CELL_H);

    mouse_x = gfx.width / 2;
    mouse_y = gfx.height / 2;
    gfx_cursor_move(&gfx, mouse_x, mouse_y);

    build_desktop();
    ctl_open();
    app_init(&desk, &gfx);
    update_clock();
    gfx_damage(&gfx, rect_make(0, 0, gfx.width, gfx.height));

    struct input_event events[32];
    while (running) {
        struct pollfd fds[WIN_MAX + 2 + CTL_MAX + APP_MAX + 1];
        int n = 0;
        fds[n].fd = input_fd;
        fds[n].events = POLLIN;
        n++;
        int ctl_first = n;
        fds[n].fd = ctl_fd;
        fds[n].events = POLLIN;
        n++;
        for (int i = 0; i < ctl_count; i++) {
            fds[n].fd = ctl_clients[i];
            fds[n].events = POLLIN;
            n++;
        }
        for (int i = 0; i < desk.count && n < WIN_MAX + 1; i++) {
            struct window *w = &desk.windows[desk.order[i]];
            if (w->kind == WIN_TERM && w->master >= 0) {
                fds[n].fd = w->master;
                fds[n].events = POLLIN;
                n++;
            }
        }
        int app_first = n;
        n += app_pollfds(fds + n, APP_MAX + 1);
        poll(fds, (nfds_t)n, POLL_TIMEOUT);

        if (fds[0].revents & POLLIN) {
            long got = read(input_fd, events, sizeof(events));
            for (long i = 0; i < got / (long)sizeof(events[0]); i++)
                handle_event(&events[i]);
        }
        for (int i = ctl_count - 1; i >= 0; i--)
            if (fds[ctl_first + 1 + i].revents & (POLLIN | POLLHUP))
                ctl_serve(i);
        if (ctl_fd >= 0 && (fds[ctl_first].revents & POLLIN))
            ctl_accept();
        app_service(fds + app_first, n - app_first);
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
