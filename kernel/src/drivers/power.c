#include <drivers/power.h>

#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/context.h>

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <fs/block.h>
#include <fs/vfs.h>
#include <kernel/log.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <lib/string.h>

static bool ready;
static bool button;
static uacpi_pci_routing_table *prt;

static uacpi_iteration_decision find_root(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
    (void)depth;
    *(uacpi_namespace_node **)user = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

static void power_button_work(uacpi_handle ctx)
{
    (void)ctx;
    log_info("acpi", "botao de energia apertado: desligando");
    power_off();
}

static uacpi_interrupt_ret power_button(uacpi_handle ctx)
{
    (void)ctx;
    if (!button) {
        button = true;
        uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, power_button_work, NULL);
    }
    return UACPI_INTERRUPT_HANDLED;
}

static bool step(uacpi_status st, const char *what)
{
    if (st == UACPI_STATUS_OK)
        return true;
    log_warn("acpi", "uACPI: %s falhou: %s", what, uacpi_status_to_string(st));
    return false;
}

void power_init(void)
{
    acpi_os_init();
    uacpi_context_set_log_level(UACPI_LOG_WARN);
    uint64_t t0 = time_now_us();
    if (!step(uacpi_initialize(0), "uacpi_initialize"))
        return;
    if (!step(uacpi_namespace_load(), "carregar o DSDT/SSDT"))
        return;
    if (irq_get_mode() == IRQ_MODE_IOAPIC)
        step(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC), "_PIC");
    if (!step(uacpi_namespace_initialize(), "inicializar o namespace"))
        return;
    step(uacpi_finalize_gpe_initialization(), "GPEs");
    bool btn = step(uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL),
                    "botao de energia");
    uacpi_namespace_node *root = NULL;
    uacpi_find_devices("PNP0A08", find_root, &root);
    if (!root)
        uacpi_find_devices("PNP0A03", find_root, &root);
    if (root && uacpi_get_pci_routing_table(root, &prt) != UACPI_STATUS_OK)
        prt = NULL;
    ready = true;
    log_info("acpi", "uACPI pronto em %lu ms: namespace carregado, botao de energia %s, _PRT com %lu rota(s)",
             (time_now_us() - t0) / 1000, btn ? "ligado" : "ausente", prt ? (uint64_t)prt->num_entries : 0);
}

bool power_ready(void)
{
    return ready;
}

struct irq_pick {
    uint32_t index;
    struct pci_irq_route *out;
    bool found;
};

static uacpi_iteration_decision pick_irq(void *user, uacpi_resource *r)
{
    struct irq_pick *p = user;
    if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs > p->index) {
        p->out->gsi = r->irq.irqs[p->index];
        p->out->level = r->irq.triggering == UACPI_TRIGGERING_LEVEL;
        p->out->active_low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        p->found = true;
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs > p->index) {
        p->out->gsi = r->extended_irq.irqs[p->index];
        p->out->level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        p->out->active_low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        p->found = true;
    }
    return p->found ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

bool power_pci_route(uint8_t slot, uint8_t pin, struct pci_irq_route *out)
{
    if (!prt || pin < 1 || pin > 4)
        return false;
    for (uacpi_size i = 0; i < prt->num_entries; i++) {
        uacpi_pci_routing_table_entry *e = &prt->entries[i];
        if ((e->address >> 16) != slot || e->pin != pin - 1)
            continue;
        if (!e->source) {
            out->gsi = e->index;
            out->level = true;
            out->active_low = true;
            return true;
        }
        uacpi_resources *res = NULL;
        if (uacpi_get_current_resources(e->source, &res) != UACPI_STATUS_OK)
            return false;
        struct irq_pick p = { 0, out, false };
        uacpi_for_each_resource(res, pick_irq, &p);
        uacpi_free_resources(res);
        return p.found;
    }
    return false;
}

static void flush_disks(void)
{
    if (!interrupts_enabled())
        return;
    uint64_t before = blockdev_flush_total();
    bool ok = vfs_sync();
    log_info("acpi", "discos gravados (%lu FLUSH)%s", blockdev_flush_total() - before, ok ? "" : ", com erro");
}

void power_off(void)
{
    log_info("acpi", "desligando: gravando os discos");
    flush_disks();
    if (ready && step(uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5), "preparar o S5")) {
        log_info("acpi", "entrando no estado S5");
        cli();
        smp_halt_others();
        step(uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5), "entrar no S5");
    }
    log_warn("acpi", "nao deu para desligar pelo ACPI; parando a CPU");
    power_halt();
}

void power_reboot(void)
{
    log_info("acpi", "reiniciando: gravando os discos");
    flush_disks();
    if (ready) {
        log_info("acpi", "reiniciando pelo registrador de reset do FADT");
        step(uacpi_reboot(), "reiniciar pelo FADT");
        time_udelay(100000);
    }
    log_warn("acpi", "reiniciando pelo controlador do teclado");
    cli();
    outb(0x64, 0xFE);
    time_udelay(100000);
    struct {
        uint16_t limit;
        uint64_t base;
    } PACKED null_idt = { 0, 0 };
    __asm__ volatile("lidt %0\n\tint3" : : "m"(null_idt));
    halt_forever();
}

void power_halt(void)
{
    cli();
    smp_halt_others();
    log_info("acpi", "sistema parado");
    halt_forever();
}
