#include "gui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <orion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WM_SOCKET "/run/wm.sock"
#define WM_CONF   "/etc/wm.conf"
#define DEFAULT   "/usr/share/wallpapers/orion.png"
#define MAX_PICS  32

static struct gui g;
static struct gui_widget *sys_label, *cpu_label, *mem_label, *net_label, *list, *status;
static char pics[MAX_PICS][128];
static char rows[MAX_PICS][48];
static int npics;
static char current[256] = DEFAULT;

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

static unsigned long meminfo_kb(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    return p ? strtoul(p + strlen(key), NULL, 10) : 0;
}

static bool is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static void system_info(void)
{
    char buf[1024];
    if (read_text("/proc/version", buf, sizeof(buf)) > 0) {
        char name[32] = "", ver[32] = "", arch[32] = "";
        sscanf(buf, "%31s version %31s (%31[^)])", name, ver, arch);
        gui_set_text(sys_label, "%s %s %s", name, ver, arch);
    }
    char cpu[4096];
    int cpus = 0;
    char model[64] = "?";
    if (read_text("/proc/cpuinfo", cpu, sizeof(cpu)) > 0) {
        for (char *p = cpu; (p = strstr(p, "processor")); p++)
            cpus++;
        char *m = strstr(cpu, "model name");
        if (m && (m = strchr(m, ':'))) {
            m += 2;
            snprintf(model, sizeof(model), "%.*s", (int)strcspn(m, "\n"), m);
        }
    }
    gui_set_text(cpu_label, "%d x %s", cpus, model);
    unsigned long secs = read_text("/proc/uptime", buf, sizeof(buf)) > 0 ? strtoul(buf, NULL, 10) : 0;
    if (read_text("/proc/meminfo", buf, sizeof(buf)) > 0)
        gui_set_text(mem_label, "%lu MiB, %lu livres, ligado %02lu:%02lu:%02lu", meminfo_kb(buf, "MemTotal:") / 1024,
                     meminfo_kb(buf, "MemFree:") / 1024, secs / 3600, secs / 60 % 60, secs % 60);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq_orion r;
    memset(&r, 0, sizeof(r));
    if (fd >= 0 && ioctl(fd, SIOCGIFCONF_ORION, &r) == 0) {
        char ip[16];
        gui_set_text(net_label, "Rede: eth0 %s, link %s", orion_inet_ntoa(r.ip, ip, sizeof(ip)),
                     r.link_up ? "up" : "down");
    } else {
        gui_set_text(net_label, "Rede: sem interface");
    }
    if (fd >= 0)
        close(fd);
}

static void read_current(void)
{
    char buf[512];
    if (read_text(WM_CONF, buf, sizeof(buf)) <= 0)
        return;
    char *p = strstr(buf, "fundo=");
    if (p && (p == buf || p[-1] == '\n'))
        snprintf(current, sizeof(current), "%.*s", (int)strcspn(p + 6, "\r\n"), p + 6);
}

static int by_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && npics < MAX_PICS)
        if (is_image(e->d_name))
            snprintf(pics[npics++], sizeof(pics[0]), "%s/%s", dir, e->d_name);
    if (d)
        closedir(d);
}

static void fill_list(void)
{
    npics = 0;
    snprintf(pics[npics++], sizeof(pics[0]), "nenhum");
    scan("/usr/share/imagens");
    scan("/usr/share/wallpapers");
    qsort(pics + 1, (size_t)(npics - 1), sizeof(pics[0]), by_path);
    gui_list_clear(list);
    for (int i = 0; i < npics; i++) {
        const char *shown = i == 0 ? "degrade, sem imagem" : pics[i] + strlen("/usr/share/");
        snprintf(rows[i], sizeof(rows[i]), "%s %s", strcmp(pics[i], current) ? " " : "*", shown);
        gui_list_add(list, rows[i]);
        if (!strcmp(pics[i], current))
            list->selected = i;
    }
    list->version++;
}

static bool wm_command(const char *cmd, char *reply, size_t size)
{
    reply[0] = 0;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { 0 };
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", WM_SOCKET);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        snprintf(reply, size, "wm: %s", strerror(errno));
        if (s >= 0)
            close(s);
        return false;
    }
    char line[300];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    bool ok = write(s, line, (size_t)n) == n;
    shutdown(s, SHUT_WR);
    size_t used = 0;
    ssize_t r;
    while (ok && used + 1 < size && (r = read(s, reply + used, size - 1 - used)) > 0)
        used += (size_t)r;
    reply[used] = 0;
    reply[strcspn(reply, "\n")] = 0;
    close(s);
    return ok && strncmp(reply, "ok", 2) == 0;
}

static void on_apply(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    if (list->selected < 0 || list->selected >= npics)
        return;
    const char *pick = pics[list->selected];
    char cmd[200], reply[256];
    snprintf(cmd, sizeof(cmd), "fundo %s", pick);
    if (!wm_command(cmd, reply, sizeof(reply))) {
        gui_set_text(status, "%s", reply);
        return;
    }
    FILE *f = fopen(WM_CONF, "w");
    if (!f || fprintf(f, "fundo=%s\n", pick) < 0 || fclose(f) != 0) {
        gui_set_text(status, "%s: %s", WM_CONF, strerror(errno));
        return;
    }
    snprintf(current, sizeof(current), "%s", pick);
    const char *slash = strrchr(pick, '/');
    const char *name = slash ? slash + 1 : pick;
    fill_list();
    gui_set_text(status, "papel de parede: %s", name);
    gui_set_title(gu, "config: fundo %s", name);
}

static void on_close(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    gui_quit(gu);
}

static void on_tick(struct gui *gu)
{
    (void)gu;
    system_info();
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
    if (!gui_open(&g, 640, 400, "config"))
        return 1;
    g.on_key = on_key;
    g.on_tick = on_tick;
    g.tick_ms = 1000;
    gui_label(&g, 10, 10, 620, "Sistema");
    sys_label = gui_label(&g, 26, 34, 604, "");
    cpu_label = gui_label(&g, 26, 56, 604, "");
    mem_label = gui_label(&g, 26, 78, 604, "");
    net_label = gui_label(&g, 26, 100, 604, "");
    gui_label(&g, 10, 132, 620, "Papel de parede (Enter aplica)");
    list = gui_list(&g, 10, 156, 620, 8, NULL);
    list->open = on_apply;
    gui_button(&g, 10, 330, 200, "Aplicar", on_apply);
    gui_button(&g, 430, 330, 200, "Fechar", on_close);
    status = gui_label(&g, 10, 368, 620, "");
    read_current();
    fill_list();
    system_info();
    gui_set_title(&g, "Configuracoes");
    gui_focus(&g, list);
    gui_run(&g);
    return 0;
}
