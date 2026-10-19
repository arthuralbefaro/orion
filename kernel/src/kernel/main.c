#include <drivers/power.h>
#include <kernel/version.h>
#include <kernel/log.h>
#include <kernel/shell.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/smp.h>
#include <drivers/pci.h>
#include <drivers/e1000.h>
#include <net/net.h>
#include <drivers/ahci.h>
#include <drivers/virtio.h>
#include <drivers/nvme.h>
#include <drivers/usb.h>
#include <fs/vfs.h>
#include <fs/block.h>
#include <fs/devfs.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <drivers/tty.h>
#include <drivers/rtc.h>
#include <kernel/process.h>
#include <arch/x86_64/percpu.h>
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
#include <drivers/mouse.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <limine.h>
#include <arch/x86_64/fpu.h>
#include <arch/x86_64/protect.h>

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
    if (boot_info.kernel_partition)
        log_info("boot", "kernel lido de %s no %s, particao %u", boot_info.kernel_path,
                 boot_info.kernel_media, boot_info.kernel_partition);
    else
        log_info("boot", "kernel lido de %s no %s, sem particao", boot_info.kernel_path,
                 boot_info.kernel_media);
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

    if (acpi_init() && ioapic_init()) {
        pic_disable();
        lapic_set_lint0_extint(false);
        irq_set_mode(IRQ_MODE_IOAPIC);
        log_info("main", "IRQs via I/O APIC");
    } else {
        irq_set_mode(IRQ_MODE_PIC);
        log_info("main", "IRQs via 8259 (modo legado)");
    }

    percpu_init_bsp(lapic_id());
    time_init();
    rtc_init();
    keyboard_init();
    mouse_init();
    sched_init();
    syscall_init();
    fpu_init_cpu();
    cpu_protect_init();
    log_info("boot", "%u modulo(s) carregado(s)", boot_info.module_count);

    sti();
    smp_init();
    heap_start_reclaimer();
    pci_init();
    power_init();
    ahci_init();
    virtio_blk_init();
    nvme_init();
    e1000_init();
    virtio_net_init();
    usb_init();
    net_init();
    blockdev_cache_init();
    vfs_init();
    vfs_flush_init();
    devfs_init();
    tmpfs_init();
    procfs_init();
    vfs_mount_extra();
    cli();

    sti();
    log_info("main", "interrupcoes habilitadas");

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

    tty_init();
    process_alarm_init();
    bool kernel_shell = true;
    if (vfs_mounted() && process_spawn_path("/bin/init", NULL)) {
        kernel_shell = false;
        log_info("main", "init iniciado; Ctrl+K alterna para o shell do kernel");
    } else {
        log_warn("main", "/bin/init nao encontrado; usando o shell do kernel");
        shell_init();
    }

    for (;;) {
        int c = keyboard_getchar();
        if (c < 0)
            c = serial_getc();
        if (c == 0x0B) {
            kernel_shell = !kernel_shell;
            if (kernel_shell)
                shell_init();
            else
                kprintf("\n[tty]\n");
        } else if (c >= 0) {
            if (kernel_shell)
                shell_feed((char)c);
            else
                tty_input((char)c);
        } else if (irqs_ok) {
            hlt();
        } else {
            pause();
        }
    }
}