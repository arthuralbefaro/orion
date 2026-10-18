#include "gui.h"

#include <stdio.h>
#include <string.h>

#define ITEMS 40

static struct gui g;
static struct gui_widget *name, *greeting, *counter, *list, *dark;
static char item_text[ITEMS + 16][24];
static int items, sums;

static void add_item(const char *text)
{
    if (items >= (int)(sizeof(item_text) / sizeof(item_text[0])))
        return;
    snprintf(item_text[items], sizeof(item_text[items]), "%s", text);
    gui_list_add(list, item_text[items]);
    items++;
}

static void on_name(struct gui *gu, struct gui_widget *w)
{
    gui_set_text(greeting, "Ola, %s!", w->text[0] ? w->text : "pessoa");
    gui_set_title(gu, "guidemo: ola %s", w->text);
}

static void on_sum(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    sums++;
    gui_set_text(counter, "Contador: %d", sums);
    gui_set_title(gu, "guidemo: somar %d", sums);
}

static void on_dark(struct gui *gu, struct gui_widget *w)
{
    gui_set_title(gu, "guidemo: caixa %s", w->checked ? "ligada" : "desligada");
}

static void on_select(struct gui *gu, struct gui_widget *w)
{
    if (w->selected >= 0)
        gui_set_title(gu, "guidemo: item %s", w->items[w->selected]);
}

static void on_add(struct gui *gu, struct gui_widget *w)
{
    (void)w;
    char text[24];
    snprintf(text, sizeof(text), "%s", name->text[0] ? name->text : "Novo");
    add_item(text);
    gui_set_title(gu, "guidemo: lista com %d", list->count);
}

static void on_menu(struct gui *gu, int menu, int item)
{
    if (menu == 0 && item == 0) {
        add_item("Do menu");
        gui_set_title(gu, "guidemo: lista com %d", list->count);
    } else if (menu == 0 && item == 1) {
        gui_list_clear(list);
        items = 0;
        gui_set_title(gu, "guidemo: lista limpa");
    } else if (menu == 0 && item == 2) {
        gui_set_title(gu, "guidemo: saindo");
        gui_quit(gu);
    } else if (menu == 1) {
        gui_set_text(greeting, "libgui do Orion");
        gui_set_title(gu, "guidemo: sobre");
    }
}

int main(void)
{
    if (!gui_open(&g, 600, 400, "guidemo"))
        return 1;
    static const char *const file_items[] = { "Novo item", "Limpar lista", "Sair" };
    static const char *const help_items[] = { "Sobre" };
    gui_menu(&g, "Arquivo", file_items, 3, on_menu);
    gui_menu(&g, "Ajuda", help_items, 1, on_menu);

    gui_label(&g, 16, 40, 80, "Nome:");
    name = gui_text(&g, 100, 36, 240, "", on_name);
    gui_button(&g, 360, 36, 120, "Somar", on_sum);
    dark = gui_check(&g, 16, 72, 300, "Caixa de opcao", on_dark);
    list = gui_list(&g, 16, 108, 300, 12, on_select);
    gui_button(&g, 340, 108, 200, "Adicionar", on_add);
    greeting = gui_label(&g, 340, 150, 250, "Ola!");
    counter = gui_label(&g, 340, 180, 250, "Contador: 0");
    gui_label(&g, 340, 230, 250, "Tab troca o foco");
    gui_label(&g, 340, 254, 250, "Enter ativa");
    gui_label(&g, 340, 278, 250, "F10 abre o menu");
    (void)dark;

    for (int i = 1; i <= ITEMS; i++) {
        char t[24];
        snprintf(t, sizeof(t), "Item %d", i);
        add_item(t);
    }
    gui_focus(&g, name);
    printf("guidemo: janela %d aberta com %d componentes\n", g.id, g.count);
    gui_run(&g);
    printf("guidemo: fim\n");
    return 0;
}
