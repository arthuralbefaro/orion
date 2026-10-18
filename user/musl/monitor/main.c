#include "gui.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HIST       60
#define GRAPH_W    300
#define GRAPH_H    110
#define COL_GRAPH  0x141A26
#define COL_GRID   0x2C3648
#define COL_CPU    0x4CC870
#define COL_MEM    0xE0A040
#define COL_TITLE  0xC8D0E0

struct proc {
    int pid;
    char name[16];
    char state;
    unsigned long rss_kb;
    long ticks;
    int cpu;
};

static struct gui g;
static struct gui_widget *cpu_label, *mem_label, *graphs, *list, *status;
static struct proc procs[GUI_LIST_MAX], prev[GUI_LIST_MAX];
static int nprocs, nprev, shown_count = -1, killed_pid;
static char rows[GUI_LIST_MAX][48];
static int cpu_hist[HIST], mem_hist[HIST], nhist;
static unsigned long long last_busy, last_idle;
static long last_ms;

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned long meminfo_kb(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtoul(p + strlen(key), NULL, 10) : 0;
}

static bool read_proc(int pid, struct proc *p)
{
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_text(path, buf, sizeof(buf)) < 0)
        return false;
    char *open_paren = strchr(buf, '(');
    char *close_paren = strrchr(buf, ')');
    if (!open_paren || !close_paren || close_paren < open_paren || !close_paren[1])
        return false;
    p->pid = pid;
    snprintf(p->name, sizeof(p->name), "%.*s", (int)(close_paren - open_paren - 1), open_paren + 1);
    p->state = close_paren[2];
    long utime = 0, stime = 0;
    unsigned long rss = 0;
    int field = 3;
    char *save = NULL;
    for (char *tok = strtok_r(close_paren + 2, " ", &save); tok; tok = strtok_r(NULL, " ", &save), field++) {
        if (field == 14)
            utime = atol(tok);
        else if (field == 15)
            stime = atol(tok);
        else if (field == 24)
            rss = strtoul(tok, NULL, 10);
    }
    p->ticks = utime + stime;
    p->rss_kb = rss * 4;
    p->cpu = 0;
    return true;
}

static int by_pid(const void *a, const void *b)
{
    return ((const struct proc *)a)->pid - ((const struct proc *)b)->pid;
}

static void scan_procs(long elapsed_ms)
{
    memcpy(prev, procs, sizeof(procs[0]) * (size_t)nprocs);
    nprev = nprocs;
    nprocs = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)) && nprocs < GUI_LIST_MAX)
        if (isdigit((unsigned char)e->d_name[0]) && read_proc(atoi(e->d_name), &procs[nprocs]))
            nprocs++;
    if (d)
        closedir(d);
    qsort(procs, (size_t)nprocs, sizeof(procs[0]), by_pid);
    for (int i = 0; i < nprocs; i++)
        for (int j = 0; j < nprev && elapsed_ms > 0; j++)
            if (prev[j].pid == procs[i].pid && procs[i].ticks >= prev[j].ticks) {
                procs[i].cpu = (int)((procs[i].ticks - prev[j].ticks) * 1000 / elapsed_ms);
                break;
            }
}

static void push(int *hist, int v)
{
    if (nhist == HIST)
        memmove(hist, hist + 1, sizeof(hist[0]) * (HIST - 1));
    hist[nhist == HIST ? HIST - 1 : nhist] = v;
}

static int sample_cpu(int *cpus)
{
    char buf[2048];
    *cpus = 0;
    if (read_text("/proc/stat", buf, sizeof(buf)) < 0)
        return 0;
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0;
    sscanf(buf, "cpu %llu %llu %llu %llu", &user, &nice, &sys, &idle);
    for (char *p = buf; (p = strstr(p, "\ncpu")); p++)
        (*cpus)++;
    unsigned long long busy = user + nice + sys;
    unsigned long long db = busy - last_busy, di = idle - last_idle;
    bool first = last_busy == 0 && last_idle == 0;
    last_busy = busy;
    last_idle = idle;
    if (first || db + di == 0)
        return 0;
    return (int)(db * 1000 / (db + di));
}

static void sample(void)
{
    long now = now_ms();
    long elapsed = last_ms ? now - last_ms : 0;
    last_ms = now;

    int cpus;
    int cpu = sample_cpu(&cpus);
    char mem[1024], up[64];
    unsigned long total = 0, free_kb = 0, cached = 0;
    if (read_text("/proc/meminfo", mem, sizeof(mem)) >= 0) {
        total = meminfo_kb(mem, "MemTotal:");
        free_kb = meminfo_kb(mem, "MemFree:");
        cached = meminfo_kb(mem, "Cached:");
    }
    unsigned long used = total > free_kb ? total - free_kb : 0;
    int mem_pct = total ? (int)(used * 1000 / total) : 0;
    unsigned long secs = read_text("/proc/uptime", up, sizeof(up)) >= 0 ? strtoul(up, NULL, 10) : 0;

    push(cpu_hist, cpu);
    push(mem_hist, mem_pct);
    if (nhist < HIST)
        nhist++;
    graphs->version++;

    gui_set_text(cpu_label, "CPU %d.%d%%  %d CPU(s)  ligado %02lu:%02lu:%02lu", cpu / 10, cpu % 10, cpus,
                 secs / 3600, secs / 60 % 60, secs % 60);
    gui_set_text(mem_label, "Mem %lu de %lu MiB (%d%%) cache %lu", used / 1024, total / 1024, mem_pct / 10,
                 cached / 1024);

    int selected_pid = list->selected >= 0 && list->selected < nprocs ? procs[list->selected].pid : -1;
    int top = list->top;
    scan_procs(elapsed);
    list->count = 0;
    list->selected = -1;
    for (int i = 0; i < nprocs; i++) {
        struct proc *p = &procs[i];
        snprintf(rows[i], sizeof(rows[i]), "%5d %-12.12s %c %6luK %3d%%", p->pid, p->name, p->state, p->rss_kb,
                 p->cpu / 10);
        gui_list_add(list, rows[i]);
        if (p->pid == selected_pid)
            list->selected = i;
    }
    list->top = top < nprocs ? top : 0;
    list->version++;
    if (killed_pid) {
        bool alive = false;
        for (int i = 0; i < nprocs; i++)
            if (procs[i].pid == killed_pid && procs[i].state != 'Z')
                alive = true;
        if (!alive) {
            gui_set_text(status, "pid %d saiu", killed_pid);
            gui_set_title(&g, "monitor: pid %d saiu", killed_pid);
            killed_pid = 0;
            shown_count = nprocs;
        }
    }
    if (nprocs != shown_count) {
        shown_count = nprocs;
        gui_set_title(&g, "monitor: %d processos", nprocs);
    }
}

static void graph(struct gui *gu, int x, int y, const int *hist, uint32_t col, const char *title)
{
    gui_fill(gu, x, y, GRAPH_W, GRAPH_H, COL_GRAPH);
    for (int i = 1; i < 4; i++)
        gui_fill(gu, x, y + GRAPH_H * i / 4, GRAPH_W, 1, COL_GRID);
    int bw = GRAPH_W / HIST;
    for (int i = 0; i < nhist; i++) {
        int bh = hist[i] * (GRAPH_H - 2) / 1000;
        if (hist[i] > 0 && bh < 1)
            bh = 1;
        gui_fill(gu, x + GRAPH_W - (nhist - i) * bw, y + GRAPH_H - bh, bw - 1, bh, col);
    }
    gui_draw_text(gu, x + 6, y + 4, title, COL_TITLE);
}

static void paint(struct gui *gu, struct gui_widget *w)
{
    graph(gu, w->x, w->y, cpu_hist, COL_CPU, "CPU");
    graph(gu, w->x + w->w - GRAPH_W, w->y, mem_hist, COL_MEM, "Memoria");
}

static void on_tick(struct gui *gu)
{
    (void)gu;
    sample();
}

static void on_kill(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    if (list->selected < 0 || list->selected >= nprocs) {
        gui_set_text(status, "escolha um processo na lista");
        return;
    }
    struct proc *p = &procs[list->selected];
    if (p->pid <= 1 || p->pid == getpid()) {
        gui_set_text(status, "%s (pid %d) fica", p->name, p->pid);
        return;
    }
    if (kill(p->pid, SIGTERM) < 0) {
        gui_set_text(status, "pid %d: %s", p->pid, strerror(errno));
        return;
    }
    killed_pid = p->pid;
    gui_set_text(status, "SIGTERM para %s (pid %d)", p->name, p->pid);
    gui_set_title(gu, "monitor: TERM %d %s", p->pid, p->name);
}

static void on_close(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    gui_quit(gu);
}

static bool on_key(struct gui *gu, int ascii, int scancode, int mods)
{
    (void)scancode;
    if (!(mods & GUI_MOD_EXTENDED) && ascii == 'q') {
        gui_quit(gu);
        return true;
    }
    return false;
}

int main(void)
{
    if (!gui_open(&g, 640, 470, "monitor"))
        return 1;
    g.on_key = on_key;
    g.on_tick = on_tick;
    g.tick_ms = 1000;
    cpu_label = gui_label(&g, 10, 10, 620, "");
    graphs = gui_canvas(&g, 10, 36, 620, GRAPH_H, paint, NULL);
    mem_label = gui_label(&g, 10, 154, 620, "");
    gui_label(&g, 16, 182, 614, "  pid nome         e     mem  cpu");
    list = gui_list(&g, 10, 204, 620, 9, NULL);
    gui_button(&g, 10, 398, 200, "Encerrar", on_kill);
    gui_button(&g, 430, 398, 200, "Fechar", on_close);
    status = gui_label(&g, 10, 436, 620, "Tab, setas e Enter; q fecha");
    sample();
    gui_run(&g);
    return 0;
}
