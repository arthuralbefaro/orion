#include "gui.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct entry {
    char name[64];
    bool dir;
    long long size;
};

static struct gui g;
static struct gui_widget *path_field, *list, *name_field, *status;
static char cwd[256];
static struct entry entries[GUI_LIST_MAX];
static char rows[GUI_LIST_MAX][48];
static int nentries;
static char pending_delete[64];

static bool is_image(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void parent_of(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        snprintf(path, 2, "/");
    else
        *slash = 0;
}

static int by_kind_and_name(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (!strcmp(x->name, ".."))
        return -1;
    if (!strcmp(y->name, ".."))
        return 1;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void format_size(char *out, size_t size, long long n)
{
    if (n < 10000)
        snprintf(out, size, "%lld", n);
    else if (n < 10000LL * 1024)
        snprintf(out, size, "%lldK", n / 1024);
    else
        snprintf(out, size, "%lldM", n / (1024 * 1024));
}

static void fill_list(const char *select)
{
    gui_list_clear(list);
    int dirs = 0, files = 0;
    for (int i = 0; i < nentries; i++) {
        struct entry *e = &entries[i];
        char size[16];
        if (e->dir) {
            snprintf(size, sizeof(size), "pasta");
            if (strcmp(e->name, ".."))
                dirs++;
        } else {
            format_size(size, sizeof(size), e->size);
            files++;
        }
        char shown[64];
        snprintf(shown, sizeof(shown), "%s%s", e->name, e->dir ? "/" : "");
        snprintf(rows[i], sizeof(rows[i]), "%-24.24s %9s", shown, size);
        gui_list_add(list, rows[i]);
        if (select && !strcmp(e->name, select))
            list->selected = i;
    }
    if (list->selected >= 0) {
        int visible = (list->h - 4) / 20;
        if (list->selected >= visible)
            list->top = list->selected - visible + 1;
    }
    list->version++;
    gui_set_text(status, "%d pasta(s), %d arquivo(s)", dirs, files);
}

static bool load(const char *path, const char *select)
{
    DIR *d = opendir(path);
    if (!d) {
        gui_set_text(status, "%s: %s", path, strerror(errno));
        return false;
    }
    char base[256];
    snprintf(base, sizeof(base), "%s", path);
    size_t len = strlen(base);
    while (len > 1 && base[len - 1] == '/')
        base[--len] = 0;
    nentries = 0;
    if (strcmp(base, "/")) {
        snprintf(entries[0].name, sizeof(entries[0].name), "..");
        entries[0].dir = true;
        entries[0].size = 0;
        nentries = 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) && nentries < GUI_LIST_MAX) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        struct entry *en = &entries[nentries];
        snprintf(en->name, sizeof(en->name), "%s", e->d_name);
        char full[512];
        join(full, sizeof(full), base, e->d_name);
        struct stat st;
        en->dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        en->size = en->dir ? 0 : (long long)st.st_size;
        nentries++;
    }
    closedir(d);
    qsort(entries, (size_t)nentries, sizeof(entries[0]), by_kind_and_name);
    snprintf(cwd, sizeof(cwd), "%s", base);
    gui_set_text(path_field, "%s", cwd);
    pending_delete[0] = 0;
    fill_list(select);
    gui_set_title(&g, "arquivos: %s", cwd);
    return true;
}

static void go_up(void)
{
    if (!strcmp(cwd, "/"))
        return;
    char up[256], child[64];
    snprintf(up, sizeof(up), "%s", cwd);
    const char *slash = strrchr(cwd, '/');
    snprintf(child, sizeof(child), "%s", slash ? slash + 1 : "");
    parent_of(up);
    load(up, child);
}

static void spawn(const char *prog, const char *arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        const char *name = strrchr(prog, '/');
        execl(prog, name ? name + 1 : prog, arg, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        gui_set_text(status, "fork: %s", strerror(errno));
}

static void on_open(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    if (list->selected < 0 || list->selected >= nentries)
        return;
    struct entry *e = &entries[list->selected];
    if (!strcmp(e->name, "..")) {
        go_up();
        return;
    }
    char full[512];
    join(full, sizeof(full), cwd, e->name);
    if (e->dir) {
        load(full, NULL);
    } else if (is_image(e->name)) {
        spawn("/bin/imgview", full);
        gui_set_text(status, "abrindo %s no imgview", e->name);
    } else {
        gui_set_text(status, "%s: %lld bytes", e->name, e->size);
    }
}

static void on_select(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    pending_delete[0] = 0;
}

static void on_path(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    char path[256];
    snprintf(path, sizeof(path), "%s", w->text[0] ? w->text : "/");
    if (!load(path, NULL))
        gui_set_text(path_field, "%s", cwd);
    gui_focus(&g, list);
}

static void on_up(struct gui *gu, struct gui_widget *w)
{
    (void)gu;
    (void)w;
    go_up();
}

static void on_mkdir(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    const char *name = name_field->text;
    if (!name[0] || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) {
        gui_set_text(status, "nome de pasta invalido");
        return;
    }
    char full[512], made[64];
    join(full, sizeof(full), cwd, name);
    if (mkdir(full, 0755) < 0) {
        gui_set_text(status, "%s: %s", name, strerror(errno));
        return;
    }
    snprintf(made, sizeof(made), "%s", name);
    gui_set_text(name_field, "");
    load(cwd, made);
    gui_set_text(status, "pasta %s criada", made);
    gui_set_title(gu, "arquivos: criou %s", made);
}

static void on_delete(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    if (list->selected < 0 || list->selected >= nentries || !strcmp(entries[list->selected].name, "..")) {
        gui_set_text(status, "escolha o que apagar na lista");
        return;
    }
    struct entry *e = &entries[list->selected];
    if (strcmp(pending_delete, e->name)) {
        snprintf(pending_delete, sizeof(pending_delete), "%s", e->name);
        gui_set_text(status, "Apagar de novo apaga %s", e->name);
        return;
    }
    char full[512], gone[64];
    join(full, sizeof(full), cwd, e->name);
    snprintf(gone, sizeof(gone), "%s", e->name);
    if ((e->dir ? rmdir(full) : unlink(full)) < 0) {
        gui_set_text(status, "%s: %s", gone, strerror(errno));
        pending_delete[0] = 0;
        return;
    }
    load(cwd, NULL);
    gui_set_text(status, "%s apagado", gone);
    gui_set_title(gu, "arquivos: apagou %s", gone);
}

static void on_close(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    gui_quit(gu);
}

static void on_tick(struct gui *gu)
{
    (void)gu;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static bool on_key(struct gui *gu, int ascii, int scancode, int mods)
{
    (void)scancode;
    if (gu->focus >= 0 && gu->widgets[gu->focus].kind == GUI_TEXT)
        return false;
    if (mods & GUI_MOD_EXTENDED)
        return false;
    if (ascii == 'q') {
        gui_quit(gu);
        return true;
    }
    if (ascii == GUI_KEY_BACKSPACE) {
        go_up();
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (!gui_open(&g, 640, 440, "arquivos"))
        return 1;
    g.on_key = on_key;
    g.on_tick = on_tick;
    g.tick_ms = 1000;
    path_field = gui_text(&g, 10, 10, 620, "/", on_path);
    list = gui_list(&g, 10, 42, 620, 14, on_select);
    list->open = on_open;
    gui_label(&g, 10, 340, 80, "Nome");
    name_field = gui_text(&g, 90, 336, 330, "", on_mkdir);
    gui_button(&g, 430, 336, 200, "Nova pasta", on_mkdir);
    gui_button(&g, 10, 370, 140, "Abrir", on_open);
    gui_button(&g, 170, 370, 140, "Subir", on_up);
    gui_button(&g, 330, 370, 140, "Apagar", on_delete);
    gui_button(&g, 490, 370, 140, "Fechar", on_close);
    status = gui_label(&g, 10, 408, 620, "");
    const char *start = argc > 1 ? argv[1] : "/";
    if (!load(start, NULL))
        load("/", NULL);
    gui_focus(&g, list);
    gui_run(&g);
    return 0;
}
