#include <kernel/shell.h>
#include <kernel/log.h>
#include <kernel/version.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/lapic.h>
#include <drivers/console.h>
#include <drivers/acpi.h>
#include <drivers/ioapic.h>
#include <kernel/time.h>
#include <drivers/pic.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <mm/memory.h>
#include <lib/string.h>

#define LINE_MAX 128
#define PROMPT   "orion> "

static char line[LINE_MAX];
static size_t line_len;

struct command {
    const char *name;
    const char *help;
    void (*fn)(const char *args);
};

static void cmd_help(const char *args);

static void cmd_clear(const char *args)
{
    (void)args;
    console_clear();
}

static void cmd_echo(const char *args)
{
    kprintf("%s\n", args);
}

static void cmd_mem(const char *args)
{
    (void)args;
    struct pmm_stats st;
    pmm_get_stats(&st);
    kprintf("Paginas: %lu total, %lu usadas, %lu livres\n",
            st.total_pages, st.used_pages, st.free_pages);
    kprintf("RAM utilizavel: %lu MiB (livre: %lu MiB), bitmap: %lu KiB\n",
            st.total_pages * PAGE_SIZE / MiB(1), st.free_pages * PAGE_SIZE / MiB(1),
            st.bitmap_bytes / 1024);
    kprintf("HHDM offset: %016lx | kernel fisico: %016lx virtual: %016lx\n",
            boot_info.hhdm_offset, boot_info.kernel_phys_base, boot_info.kernel_virt_base);
}

static void cmd_memmap(const char *args)
{
    (void)args;
    pmm_dump_memmap();
}

static void cmd_alloc(const char *args)
{
    (void)args;
    uint64_t pages[4];
    for (int i = 0; i < 4; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            kprintf("alocacao %d falhou\n", i);
            return;
        }
        uint64_t *p = phys_to_virt(pages[i]);
        p[0] = 0xDEADBEEF00000000ull | (uint64_t)i;
        kprintf("  pagina %d -> %016lx (virt %p) ok=%d\n", i, pages[i], (void *)p,
                p[0] == (0xDEADBEEF00000000ull | (uint64_t)i));
    }
    for (int i = 0; i < 4; i++)
        pmm_free_page(pages[i]);
    kprintf("  4 paginas alocadas e liberadas.\n");
}

static void cmd_heap(const char *args)
{
    (void)args;
    heap_dump();
}

static void cmd_heaptest(const char *args)
{
    (void)args;
    kprintf("Auto-teste do heap...\n");
    heap_selftest();
}

static void cmd_cpu(const char *args)
{
    (void)args;
    char vendor[13], brand[49];
    cpu_get_vendor(vendor);
    cpu_get_brand(brand);
    kprintf("Vendor: %s\nModelo: %s\nLAPIC: %s (id %u)\n", vendor, brand,
            !lapic_is_enabled() ? "indisponivel" : lapic_is_x2apic() ? "x2APIC" : "xAPIC",
            lapic_id());
    kprintf("CR0=%016lx CR3=%016lx CR4=%016lx EFER=%016lx\n",
            read_cr0(), read_cr3(), read_cr4(), rdmsr(MSR_IA32_EFER));
}

static void cmd_uptime(const char *args)
{
    (void)args;
    uint64_t ms = time_uptime_ms();
    uint64_t us = time_now_us();
    kprintf("Uptime: %lu.%03lu s (%lu ticks a %u Hz via %s)\n",
            ms / 1000, ms % 1000, time_ticks(), TIME_TICK_HZ, time_source_name());
    kprintf("TSC: %lu.%06lu s (%lu MHz)\n", us / 1000000, us % 1000000, time_tsc_hz() / 1000000);
}

static void cmd_irq(const char *args)
{
    (void)args;
        kprintf("Modo: %s | mascara do PIC: %04x\n",
            irq_get_mode() == IRQ_MODE_IOAPIC ? "I/O APIC" : "8259 PIC", pic_get_mask());
    if (irq_get_mode() == IRQ_MODE_IOAPIC)
        ioapic_dump();
}

static void cmd_fb(const char *args)
{
    (void)args;
    const struct framebuffer *fb = console_get_framebuffer();
    if (!fb) {
        kprintf("sem framebuffer\n");
        return;
    }
    uint32_t cols, rows;
    console_get_size(&cols, &rows);
    kprintf("Framebuffer: %lux%lu, %u bpp, pitch %lu, em %p\n",
            fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    kprintf("Console: %ux%u celulas, fonte 8x8 com zoom %ux\n",
            cols, rows, console_get_scale());
}

static void cmd_acpi(const char *args)
{
    (void)args;
    acpi_dump();
}

static void cmd_cpus(const char *args)
{
    (void)args;
    const struct acpi_madt_info *m = acpi_madt();
    if (!m) {
        kprintf("sem MADT (ACPI indisponivel)\n");
        return;
    }
    kprintf("%u CPU(s) no MADT (esta e a LAPIC %u):\n", m->cpu_count, lapic_id());
    for (uint32_t i = 0; i < m->cpu_count; i++)
        kprintf("  cpu %u: acpi_id=%u apic_id=%u %s\n", i, m->cpus[i].acpi_id,
                m->cpus[i].apic_id, m->cpus[i].enabled ? "ativa" : "desativada");
}

static void cmd_boot(const char *args)
{
    (void)args;
    kprintf("Bootloader: %s %s | firmware: %s | protocolo Limine rev %lu\n",
            boot_info.bootloader_name, boot_info.bootloader_version,
            boot_info.firmware, boot_info.base_revision);
    kprintf("RSDP (ACPI): %p\n", boot_info.rsdp);
}

static void cmd_panic(const char *args)
{
    panic("panic manual pelo shell: %s", *args ? args : "(sem mensagem)");
}

static void cmd_crash(const char *args)
{
    (void)args;
    kprintf("Provocando divisao por zero...\n");
    __asm__ volatile("xorl %%edx, %%edx\n\t"
                     "movl $1, %%eax\n\t"
                     "xorl %%ecx, %%ecx\n\t"
                     "divl %%ecx"
                     : : : "eax", "ecx", "edx", "cc");
}

static void cmd_pf(const char *args)
{
    (void)args;
    kprintf("Provocando page fault em 0xdeadbeef...\n");
    volatile uint64_t *p = (volatile uint64_t *)0xDEADBEEFull;
    *p = 42;
}

static void cmd_reboot(const char *args)
{
    (void)args;
    kprintf("Reiniciando...\n");
    outb(0x64, 0xFE);
    struct { uint16_t limit; uint64_t base; } PACKED null_idt = { 0, 0 };
    __asm__ volatile("lidt %0\n\tint3" : : "m"(null_idt));
    halt_forever();
}

static void cmd_halt(const char *args)
{
    (void)args;
    kprintf("CPU parada.\n");
    halt_forever();
}

static const struct command commands[] = {
    { "help",   "lista os comandos",                    cmd_help },
    { "clear",  "limpa a tela",                         cmd_clear },
    { "echo",   "imprime os argumentos",                cmd_echo },
    { "mem",    "estatisticas de memoria fisica",       cmd_mem },
    { "memmap", "mapa de memoria do bootloader",        cmd_memmap },
    { "alloc",  "testa o alocador de paginas",          cmd_alloc },
    { "heap",   "estatisticas do kmalloc",              cmd_heap },
    { "heaptest", "auto-teste do kmalloc/kfree",        cmd_heaptest },
    { "cpu",    "informacoes da CPU",                   cmd_cpu },
    { "uptime", "tempo desde o boot",                   cmd_uptime },
    { "irq",    "contadores de interrupcao",            cmd_irq },
    { "fb",     "informacoes do framebuffer/console",   cmd_fb },
    { "boot",   "informacoes do bootloader",            cmd_boot },
    { "acpi",   "tabelas ACPI encontradas",             cmd_acpi },
    { "cpus",   "CPUs listadas no MADT",                cmd_cpus },
    { "panic",  "dispara um kernel panic",              cmd_panic },
    { "crash",  "divisao por zero (testa excecao #DE)", cmd_crash },
    { "pf",     "page fault proposital (testa #PF)",    cmd_pf },
    { "reboot", "reinicia a maquina",                   cmd_reboot },
    { "halt",   "para a CPU",                           cmd_halt },
};

static void cmd_help(const char *args)
{
    (void)args;
    for (size_t i = 0; i < ARRAY_SIZE(commands); i++)
        kprintf("  %-8s %s\n", commands[i].name, commands[i].help);
}

static void execute(char *cmdline)
{
    while (*cmdline == ' ')
        cmdline++;
    if (!*cmdline)
        return;

    char *args = cmdline;
    while (*args && *args != ' ')
        args++;
    if (*args) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
    }

    for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
        if (strcmp(cmdline, commands[i].name) == 0) {
            commands[i].fn(args);
            return;
        }
    }
    kprintf("comando desconhecido: '%s' (tente 'help')\n", cmdline);
}

void shell_init(void)
{
    line_len = 0;
    kprintf("\n" KERNEL_NAME " v" KERNEL_VERSION " - digite 'help'.\n" PROMPT);
}

void shell_feed(char c)
{
    switch (c) {
    case '\r':
    case '\n':
        kputc('\n');
        line[line_len] = '\0';
        execute(line);
        line_len = 0;
        kputs(PROMPT);
        break;
    case '\b':
    case 0x7F:
        if (line_len > 0) {
            line_len--;
            kputs("\b \b");
        }
        break;
    case 0x03:
        kputs("^C\n" PROMPT);
        line_len = 0;
        break;
    case 0x0C:
        console_clear();
        kputs(PROMPT);
        line[line_len] = '\0';
        kputs(line);
        break;
    default:
        if (c >= 0x20 && c < 0x7F && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            kputc(c);
        }
        break;
    }
}