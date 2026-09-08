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
#include <drivers/mouse.h>
#include <drivers/input.h>
#include <drivers/pty.h>
#include <fs/devfs.h>
#include <kernel/time.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <drivers/pci.h>
#include <fs/block.h>
#include <fs/vfs.h>
#include <arch/x86_64/percpu.h>
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
    for (int v = 0; v < 256; v++) {
        uint64_t n = interrupts_count((uint8_t)v);
        if (n)
            kprintf("  vetor %3d: %lu\n", v, n);
    }
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

static void cmd_ps(const char *args)
{
    (void)args;
    sched_dump();
}

static void worker_sleepy(void *arg)
{
    uint64_t id = (uint64_t)arg;
    for (int i = 1; i <= 4; i++) {
        kprintf("  [thread %lu] iteracao %d (t=%lu ms)\n", id, i, time_uptime_ms());
        thread_sleep_ms(200 * id);
    }
    kprintf("  [thread %lu] terminando\n", id);
}

static void cmd_threads(const char *args)
{
    (void)args;
    kprintf("Criando 3 threads que dormem 200/400/600 ms entre mensagens...\n");
    for (uint64_t i = 1; i <= 3; i++) {
        char name[16];
        ksnprintf(name, sizeof(name), "sleepy-%lu", i);
        thread_create(name, worker_sleepy, (void *)i);
    }
}

static volatile uint64_t busy_count[2];
static volatile uint32_t busy_cpu[2];
static volatile bool busy_stop;

static void worker_busy(void *arg)
{
    uint64_t idx = (uint64_t)arg;
    while (!busy_stop) {
        busy_count[idx]++;
        if ((busy_count[idx] & 0xFFFF) == 0)
            busy_cpu[idx] = percpu_cpu_id();
    }
}

static void cmd_preempt(const char *args)
{
    (void)args;
    busy_count[0] = busy_count[1] = 0;
    busy_stop = false;
    struct thread *a = thread_create("busy-A", worker_busy, (void *)0);
    struct thread *b = thread_create("busy-B", worker_busy, (void *)1);
    kprintf("Duas threads em loop infinito (tids %u e %u); dormindo 1 s...\n", a->tid, b->tid);
    thread_sleep_ms(1000);
    busy_stop = true;
    thread_sleep_ms(50); 
    kprintf("  A contou %lu (cpu %u), B contou %lu (cpu %u) -> %s\n",
            busy_count[0], busy_cpu[0], busy_count[1], busy_cpu[1],
            (busy_count[0] && busy_count[1]) ? "OK (as duas rodaram)" : "FALHA");
}

static void cmd_mouse(const char *args)
{
    (void)args;
    if (!mouse_available()) {
        kprintf("mouse PS/2 nao inicializado\n");
        return;
    }
    kprintf("Eventos do mouse por 3 segundos (roda: %s)...\n", mouse_has_wheel() ? "sim" : "nao");
    uint64_t end = time_uptime_ms() + 3000;
    uint32_t seen = 0;
    while (time_uptime_ms() < end) {
        struct mouse_packet p;
        if (!mouse_poll(&p)) {
            thread_sleep_ms(10);
            continue;
        }
        seen++;
        kprintf("  dx=%4d dy=%4d dz=%2d botoes=[%c%c%c]\n", p.dx, p.dy, p.dz,
                (p.buttons & MOUSE_BUTTON_LEFT) ? 'E' : '-',
                (p.buttons & MOUSE_BUTTON_MIDDLE) ? 'M' : '-',
                (p.buttons & MOUSE_BUTTON_RIGHT) ? 'D' : '-');
    }
    kprintf("  %u evento(s) nesta janela; %lu pacote(s) e %lu resync(s) desde o boot\n",
            seen, mouse_packet_count(), mouse_resync_count());
}

static void cmd_dev(const char *args)
{
    (void)args;
    devfs_dump();
    kprintf("  /dev/input: %s, fila com %u evento(s), timeout %u ms\n",
            input_grabbed() ? "capturado por um processo" : "livre",
            input_queued(), input_timeout());
    kprintf("  %lu evento(s) enfileirado(s) e %lu descartado(s) desde o boot\n",
            input_event_count(), input_dropped_count());
}

static void cmd_pty(const char *args)
{
    (void)args;
    pty_dump();
}

static void cmd_input(const char *args)
{
    (void)args;
    kprintf("Capturando /dev/input por 3 segundos (o TTY nao recebe teclas)...\n");
    input_grab();
    input_flush();
    uint64_t end = time_uptime_ms() + 3000;
    uint32_t seen = 0;
    while (time_uptime_ms() < end) {
        struct input_event ev[8];
        input_set_timeout(200);
        int64_t n = input_read(ev, sizeof(ev));
        for (int64_t i = 0; i < n / (int64_t)sizeof(ev[0]); i++) {
            seen++;
            kprintf("  tipo=%u a=%d b=%d c=%d\n", ev[i].type, ev[i].a, ev[i].b, ev[i].c);
        }
    }
    input_release();
    kprintf("  %u evento(s)\n", seen);
}

static void cmd_mods(const char *args)
{
    (void)args;
    if (!boot_info.module_count) {
        kprintf("nenhum modulo carregado pelo bootloader\n");
        return;
    }
    for (uint32_t i = 0; i < boot_info.module_count; i++)
        kprintf("  %-12s %8lu bytes  %s\n", boot_info.modules[i].name,
                boot_info.modules[i].size, boot_info.modules[i].path);
}

static void cmd_run(const char *args)
{
    if (!*args) {
        kprintf("uso: run <modulo>   (veja 'mods')\n");
        return;
    }
    const struct boot_module *m = boot_find_module(args);
    if (!m) {
        kprintf("modulo '%s' nao encontrado\n", args);
        return;
    }
    struct process *p = process_create_elf(args, m->address, m->size);
    if (!p)
        kprintf("falha ao criar o processo\n");
}

static void cmd_procs(const char *args)
{
    (void)args;
    process_dump();
}

static void cmd_smp(const char *args)
{
    (void)args;
    smp_dump();
}

static void cmd_pci(const char *args)
{
    (void)args;
    pci_dump();
}

static void cmd_lsblk(const char *args)
{
    (void)args;
    blockdev_dump();
}

static bool ls_cb(const struct vfs_node *e, void *ctx)
{
    uint32_t *count = ctx;
    (*count)++;
    if (e->is_dir)
        kprintf("  <DIR>          %s/\n", e->name);
    else
        kprintf("  %10lu     %s\n", e->size, e->name);
    return true;
}

static void cmd_ls(const char *args)
{
    const char *path = *args ? args : "/";
    uint32_t count = 0;
    if (!vfs_readdir(path, ls_cb, &count)) {
        kprintf("ls: '%s' nao encontrado ou nao e diretorio\n", path);
        return;
    }
    kprintf("  %u entrada(s)\n", count);
}

static void cmd_cat(const char *args)
{
    if (!*args) {
        kprintf("uso: cat <arquivo>\n");
        return;
    }
    uint64_t size;
    char *data = vfs_read_all(args, &size);
    if (!data) {
        kprintf("cat: nao foi possivel ler '%s'\n", args);
        return;
    }
    for (uint64_t i = 0; i < size; i++)
        kputc(data[i]);
    if (size && data[size - 1] != '\n')
        kputc('\n');
    kfree(data);
}

static void cmd_exec(const char *args)
{
    if (!*args) {
        kprintf("uso: exec <caminho do ELF>\n");
        return;
    }
    uint64_t size;
    void *image = vfs_read_all(args, &size);
    if (!image) {
        kprintf("exec: nao foi possivel ler '%s'\n", args);
        return;
    }
    const char *name = args;
    for (const char *c = args; *c; c++)
        if (*c == '/')
            name = c + 1;
    if (!process_create_elf(name, image, size))
        kprintf("exec: falha ao criar o processo\n");
    kfree(image);
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
    { "ps",     "lista as threads",                     cmd_ps },
    { "smp",    "estado de cada CPU",                   cmd_smp },
    { "mouse",  "eventos do mouse PS/2 por 3 s",        cmd_mouse },
    { "dev",    "dispositivos de /dev",                 cmd_dev },
    { "input",  "le /dev/input por 3 s",                cmd_input },
    { "pty",    "pseudo-terminais em uso",              cmd_pty },
    { "threads", "demo: 3 threads dormindo",            cmd_threads },
    { "preempt", "demo: preempcao de loops ocupados",   cmd_preempt },
    { "mods",   "modulos carregados pelo bootloader",   cmd_mods },
    { "run",    "executa um modulo ELF em ring 3",      cmd_run },
    { "procs",  "lista os processos",                   cmd_procs },
    { "pci",    "dispositivos PCI",                     cmd_pci },
    { "lsblk",  "discos e particoes",                   cmd_lsblk },
    { "ls",     "lista um diretorio do disco",          cmd_ls },
    { "cat",    "mostra um arquivo do disco",           cmd_cat },
    { "exec",   "executa um ELF do disco em ring 3",    cmd_exec },
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
