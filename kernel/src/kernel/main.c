/*
 * main.c - ponto de entrada do kernel (chamado pelo Limine já em long mode).
 *
 * Ordem de inicialização importa:
 *   1. boot_info  - sem isso não sabemos nem onde está a memória
 *   2. serial     - primeira saída de debug
 *   3. console    - framebuffer
 *   4. GDT/IDT    - antes de qualquer coisa que possa causar exceção
 *   5. PMM/VMM/heap - memória (o LAPIC e o I/O APIC precisam de MMIO)
 *   6. PIC (mascarado) -> LAPIC -> ACPI -> I/O APIC -> timer -> teclado
 *   7. sti        - só agora interrupções ligadas
 */
#include <kernel/version.h>
#include <kernel/log.h>
#include <kernel/shell.h>
#include <kernel/time.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <drivers/serial.h>
#include <drivers/console.h>
#include <drivers/pic.h>
#include <drivers/acpi.h>
#include <drivers/ioapic.h>
#include <drivers/keyboard.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <limine.h>

static void init_console(void)
{
    struct limine_framebuffer *lfb = boot_info.framebuffer;
    if (!lfb) {
        log_warn("main", "sem framebuffer; saida apenas na serial");
        return;
    }
    struct framebuffer fb = {
        .address = lfb->address,
        .width = lfb->width,
        .height = lfb->height,
        .pitch = lfb->pitch,
        .bpp = lfb->bpp,
        .red_mask_size = lfb->red_mask_size,     .red_mask_shift = lfb->red_mask_shift,
        .green_mask_size = lfb->green_mask_size, .green_mask_shift = lfb->green_mask_shift,
        .blue_mask_size = lfb->blue_mask_size,   .blue_mask_shift = lfb->blue_mask_shift,
    };
    console_init(&fb);
}

static void print_banner(void)
{
    console_set_colors(COLOR_CYAN, COLOR_BACKGROUND);
    kprintf("\n  ___  ____  ___  ___  _  _ \n"
            " / _ \\|  _ \\|_ _|/ _ \\| \\| |\n"
            "| (_) | |_) || || (_) | .` |\n"
            " \\___/|_| \\_\\___|\\___/|_|\\_|\n");
    console_set_colors(COLOR_LIGHT_GRAY, COLOR_BACKGROUND);
    kprintf("  " KERNEL_NAME " " KERNEL_VERSION " (" KERNEL_ARCH ") - build "
            __DATE__ " " __TIME__ "\n\n");
}

void kmain(void)
{
    boot_info_init();
    serial_init();
    init_console();
    print_banner();

    log_info("boot", "%s %s via %s, protocolo Limine rev %lu",
             boot_info.bootloader_name, boot_info.bootloader_version,
             boot_info.firmware, boot_info.base_revision);
    log_info("boot", "kernel em fisico %016lx / virtual %016lx, HHDM %016lx",
             boot_info.kernel_phys_base, boot_info.kernel_virt_base, boot_info.hhdm_offset);
    if (boot_info.framebuffer)
        log_info("fb", "%lux%lu @ %u bpp", boot_info.framebuffer->width,
                 boot_info.framebuffer->height, boot_info.framebuffer->bpp);
    log_info("serial", "COM1 %s", serial_available() ? "ok (115200 8N1)" : "ausente");

    char brand[49];
    cpu_get_brand(brand);
    log_info("cpu", "%s", brand);

    gdt_init();
    idt_init();
    pmm_init();
    vmm_init();
    heap_init();
    pic_init();
    lapic_init();

    /* Com ACPI + I/O APIC, aposentamos o 8259: mascara tudo nele e fecha a
     * porta LINT0 do LAPIC. Sem ACPI (maquinas antigas), continua no PIC. */
    if (acpi_init() && ioapic_init()) {
        pic_disable();
        lapic_set_lint0_extint(false);
        irq_set_mode(IRQ_MODE_IOAPIC);
        log_info("main", "IRQs via I/O APIC");
    } else {
        irq_set_mode(IRQ_MODE_PIC);
        log_info("main", "IRQs via 8259 (modo legado)");
    }

    time_init();
    keyboard_init();

    sti();
    log_info("main", "interrupcoes habilitadas");

    /* Sem LAPIC a LINT0 continua mascarada e nenhuma IRQ do PIC chega:
     * nada de timer nem teclado (o shell ainda funciona pela serial). */
    bool irqs_ok = lapic_is_enabled();
    if (irqs_ok) {
        uint64_t t0 = time_ticks();
        uint64_t us0 = time_now_us();
        time_sleep_ms(50);
        log_info("main", "timer ok: %lu ticks em 50 ms (TSC mediu %lu us)",
                 time_ticks() - t0, time_now_us() - us0);
    } else {
        log_warn("main", "sem IRQs: use a serial para o shell");
    }

    shell_init();

    for (;;) {
        int c = keyboard_getchar();
        if (c < 0)
            c = serial_getc(); /* também aceita comandos pela serial */
        if (c >= 0)
            shell_feed((char)c);
        else if (irqs_ok)
            hlt();   /* dorme até a próxima interrupção */
        else
            pause(); /* sem interrupções, só resta fazer polling */
    }
}