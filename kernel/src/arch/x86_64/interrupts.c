#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/cpu.h>
#include <drivers/pic.h>
#include <drivers/ioapic.h>
#include <drivers/acpi.h>
#include <drivers/serial.h>
#include <arch/x86_64/lapic.h>
#include <drivers/console.h>
#include <lib/string.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <mm/memory.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/uaccess.h>
#include <mm/vma.h>
#include <mm/oom.h>
#include <kernel/prof.h>
#include <kernel/smp.h>
#include <arch/x86_64/percpu.h>
#include <kernel/irqctx.h>
#include <arch/x86_64/protect.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} PACKED;

struct idtr {
    uint16_t limit;
    uint64_t base;
} PACKED;

#define GATE_INTERRUPT 0xE
#define GATE_PRESENT   (1 << 7)
#define GATE_DPL(n)    ((n) << 5)

static struct idt_entry idt[256] ALIGNED(16);

extern uint8_t isr_stubs_begin[];
extern char __text_start[], __text_end[];
#define ISR_STUB_SIZE 16
#define ISR_STUB(vec) ((uint64_t)isr_stubs_begin + (uint64_t)(vec) * ISR_STUB_SIZE)

static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t dpl, uint8_t ist)
{
    struct idt_entry *e = &idt[vector];
    e->offset_low  = handler & 0xFFFF;
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = ist;
    e->type_attr   = GATE_PRESENT | GATE_DPL(dpl) | GATE_INTERRUPT;
    e->offset_mid  = (handler >> 16) & 0xFFFF;
    e->offset_high = (handler >> 32) & 0xFFFFFFFF;
    e->reserved    = 0;
}

struct handler_slot {
    interrupt_handler_t fn;
    void *ctx;
};

static struct handler_slot handlers[256];
static uint64_t counters[256];
static uint64_t cpu_counters[MAX_CPUS][256];
static const char *names[256];
static volatile bool storm_masked[256];
static uint32_t peak_rate[256];
static uint32_t storm_events;

void interrupts_register(uint8_t vector, interrupt_handler_t handler, void *ctx)
{
    uint64_t flags = irq_save();
    handlers[vector].ctx = ctx;
    handlers[vector].fn = handler;
    irq_restore(flags);
}

void interrupts_unregister(uint8_t vector)
{
    interrupts_register(vector, NULL, NULL);
}

uint64_t interrupts_count(uint8_t vector)
{
    return counters[vector];
}

uint64_t interrupts_count_cpu(uint8_t vector, uint32_t cpu)
{
    return cpu < MAX_CPUS ? cpu_counters[cpu][vector] : 0;
}

int interrupts_alloc_vector(interrupt_handler_t handler, void *ctx, const char *name)
{
    uint64_t flags = irq_save();
    for (int v = VECTOR_MSI_BASE; v < VECTOR_MSI_BASE + VECTOR_MSI_COUNT; v++) {
        if (handlers[v].fn)
            continue;
        handlers[v].ctx = ctx;
        handlers[v].fn = handler;
        names[v] = name;
        irq_restore(flags);
        return v;
    }
    irq_restore(flags);
    return -1;
}

const char *interrupts_name(uint8_t vector)
{
    if (names[vector])
        return names[vector];
    if (vector < VECTOR_EXCEPTIONS_END)
        return exception_name(vector);
    if (vector == VECTOR_LAPIC_TIMER)
        return "lapic-timer";
    if (vector >= VECTOR_IRQ_BASE && vector < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT)
        return "irq-isa";
    if (vector >= VECTOR_GSI_BASE && vector < VECTOR_GSI_BASE + VECTOR_GSI_COUNT)
        return "irq-gsi";
    if (vector >= VECTOR_IPI_HALT && vector <= VECTOR_IPI_RESCHED)
        return "ipi";
    if (vector == VECTOR_SPURIOUS)
        return "espuria";
    return "-";
}

uint32_t irq_next_cpu(void)
{
    static uint32_t next = 1;
    uint32_t count = percpu_count();
    if (count <= 1)
        return 0;
    for (uint32_t tries = 0; tries < MAX_CPUS; tries++) {
        uint32_t cpu = next;
        next = next + 1 >= count ? 1 : next + 1;
        struct percpu *c = percpu_by_index(cpu);
        if (c && c->online)
            return cpu;
    }
    return 0;
}

static enum irq_mode irq_mode = IRQ_MODE_PIC;

void irq_set_mode(enum irq_mode mode)
{
    irq_mode = mode;
}

enum irq_mode irq_get_mode(void)
{
    return irq_mode;
}

void irq_enable(uint8_t irq)
{
    storm_masked[VECTOR_IRQ_BASE + irq] = false;
    if (irq_mode == IRQ_MODE_IOAPIC) {
        ioapic_route_isa(irq, VECTOR_IRQ_BASE + irq, lapic_id());
        ioapic_set_mask_isa(irq, false);
    } else {
        pic_unmask(irq);
    }
}

bool irq_enable_isa_as(uint8_t irq, bool default_low, bool default_level, uint32_t *gsi, bool *low, bool *level)
{
    const struct acpi_madt_info *madt = acpi_madt();
    *low = default_low;
    *level = default_level;
    *gsi = irq;
    storm_masked[VECTOR_IRQ_BASE + irq] = false;
    if (irq_mode != IRQ_MODE_IOAPIC) {
        pic_unmask(irq);
        return true;
    }
    if (madt && irq < 16 && madt->isa_overrides[irq].present) {
        *low = madt->isa_overrides[irq].active_low;
        *level = madt->isa_overrides[irq].level_triggered;
    }
    *gsi = ioapic_isa_to_gsi(irq);
    if (!ioapic_route_gsi(*gsi, VECTOR_IRQ_BASE + irq, lapic_id(), *low, *level))
        return false;
    ioapic_set_mask_gsi(*gsi, false);
    return true;
}

bool irq_attach_gsi(uint32_t gsi, bool active_low, bool level, interrupt_handler_t handler, void *ctx)
{
    if (irq_mode != IRQ_MODE_IOAPIC || gsi < VECTOR_IRQ_COUNT || gsi >= VECTOR_IRQ_COUNT + VECTOR_GSI_COUNT)
        return false;
    uint8_t vector = (uint8_t)(VECTOR_GSI_BASE + (gsi - VECTOR_IRQ_COUNT));
    interrupts_register(vector, handler, ctx);
    if (!ioapic_route_gsi(gsi, vector, lapic_id(), active_low, level)) {
        interrupts_unregister(vector);
        return false;
    }
    storm_masked[vector] = false;
    ioapic_set_mask_gsi(gsi, false);
    return true;
}

void irq_disable(uint8_t irq)
{
    if (irq_mode == IRQ_MODE_IOAPIC)
        ioapic_set_mask_isa(irq, true);
    else
        pic_mask(irq);
}

static const char *const exception_names[32] = {
    [0]  = "#DE Divide Error",
    [1]  = "#DB Debug",
    [2]  = "NMI",
    [3]  = "#BP Breakpoint",
    [4]  = "#OF Overflow",
    [5]  = "#BR Bound Range Exceeded",
    [6]  = "#UD Invalid Opcode",
    [7]  = "#NM Device Not Available",
    [8]  = "#DF Double Fault",
    [9]  = "Coprocessor Segment Overrun",
    [10] = "#TS Invalid TSS",
    [11] = "#NP Segment Not Present",
    [12] = "#SS Stack-Segment Fault",
    [13] = "#GP General Protection",
    [14] = "#PF Page Fault",
    [15] = "Reserved",
    [16] = "#MF x87 FP Exception",
    [17] = "#AC Alignment Check",
    [18] = "#MC Machine Check",
    [19] = "#XM SIMD FP Exception",
    [20] = "#VE Virtualization",
    [21] = "#CP Control Protection",
    [22] = "Reserved", [23] = "Reserved", [24] = "Reserved", [25] = "Reserved",
    [26] = "Reserved", [27] = "Reserved",
    [28] = "#HV Hypervisor Injection",
    [29] = "#VC VMM Communication",
    [30] = "#SX Security",
    [31] = "Reserved",
};

const char *exception_name(uint8_t vector)
{
    if (vector < 32 && exception_names[vector])
        return exception_names[vector];
    return "?";
}

static bool is_text(uint64_t a)
{
    return a >= (uint64_t)__text_start && a < (uint64_t)__text_end;
}

static bool readable(uint64_t a)
{
    return vmm_virt_to_phys(a & ~(uint64_t)(PAGE_SIZE - 1)) != 0;
}

static void print_addr(uint64_t a, int *col)
{
    kprintf(*col == 0 ? "    %016lx" : " %016lx", a);
    if (++*col == 4) {
        kprintf("\n");
        *col = 0;
    }
}

static void end_row(int *col)
{
    if (*col)
        kprintf("\n");
    *col = 0;
}

static void backtrace(uint64_t rip, uint64_t rsp, uint64_t lo, uint64_t hi)
{
    kprintf("  pilha de chamadas por varredura (llvm-addr2line -f -e out\\orion.elf <endereco>):\n");
    int col = 0;
    if (is_text(rip))
        print_addr(rip, &col);
    if (rsp < lo) {
        end_row(&col);
        kprintf("    (RSP abaixo da pilha: varrendo a partir do fundo %016lx)\n", lo);
        rsp = lo;
    }
    if (rsp < hi && !(rsp & 7)) {
        int shown = 0;
        for (uint64_t *p = (uint64_t *)rsp; (uint64_t)p < hi && shown < 32; p++) {
            if (is_text(*p)) {
                print_addr(*p, &col);
                shown++;
            }
        }
    }
    end_row(&col);
}

static void frame_walk(uint64_t rbp, uint64_t lo, uint64_t hi)
{
    kprintf("  cadeia pelo RBP (do mais recente para o mais antigo):\n");
    int col = 0;
    uint32_t frames = 0, shown = 0;
    uint64_t last = 0;
    uint32_t repeats = 0;
    while (frames < 4096 && rbp >= lo && rbp + 16 <= hi && !(rbp & 7) && readable(rbp)) {
        const uint64_t *w = (const uint64_t *)rbp;
        uint64_t ret = w[1];
        if (!is_text(ret))
            break;
        frames++;
        if (ret == last) {
            repeats++;
        } else {
            if (repeats) {
                end_row(&col);
                kprintf("      (o endereco acima se repete mais %u vez(es))\n", repeats);
            }
            repeats = 0;
            if (shown < 40)
                print_addr(ret, &col);
            shown++;
            last = ret;
        }
        if (w[0] <= rbp)
            break;
        rbp = w[0];
    }
    end_row(&col);
    if (repeats)
        kprintf("      (o endereco acima se repete mais %u vez(es))\n", repeats);
    kprintf("    %u quadro(s) na cadeia%s\n", frames, shown > 40 ? " (so os 40 primeiros enderecos distintos)" : "");
}

static void overflow_frame(uint64_t rsp, uint64_t base)
{
    uint64_t lo = rsp & ~7ull;
    uint64_t hi = base + 0x100;
    for (uint64_t a = lo; a + 0x38 <= hi; a += 8) {
        if (!readable(a) || !readable(a + 0x30))
            continue;
        const uint64_t *w = (const uint64_t *)a;
        uint64_t vec = w[0], err = w[1], rip = w[2], cs = w[3], rflags = w[4], frame_rsp = w[5];
        if (vec >= 256 || !(rflags & 2) || (rflags >> 22))
            continue;
        if (cs == GDT_KERNEL_CODE) {
            if (!is_text(rip) || (frame_rsp & ~0xFull) != a + 0x38)
                continue;
        } else if ((cs & 3) == 3) {
            if (rip >= USER_ADDR_MAX || frame_rsp >= USER_ADDR_MAX)
                continue;
        } else {
            continue;
        }
        kprintf("  >>> a pilha acabou ao entrar no vetor %lu (%s) vindo de %s: RIP=%016lx RSP=%016lx ERR=%016lx <<<\n",
                vec, interrupts_name((uint8_t)vec), cs == GDT_KERNEL_CODE ? "ring 0" : "ring 3", rip,
                frame_rsp, err);
        if (cs == GDT_KERNEL_CODE && vec == EXC_PAGE_FAULT)
            kprintf("      (um #PF em ring 0 com o RSP no fim da pilha: a cadeia de chamadas abaixo e a causa)\n");
        return;
    }
}

void interrupts_dump_cpu_state(uint32_t cpu)
{
    struct percpu *c = percpu_by_index(cpu);
    if (!c || !c->online)
        return;
    kprintf("  CPU%u prof %u/%u%s vetores:", c->cpu_id, c->irq_depth, c->irq_depth_max, c->in_nmi ? " NMI" : "");
    for (uint32_t i = 0; i < IRQ_RING; i++)
        kprintf(" %u", c->irq_ring[(c->irq_ring_pos + i) % IRQ_RING]);
    kprintf(" | mais: %u(%s)x%u, antes %u(%s)x%u\n", c->irq_top_vector, interrupts_name(c->irq_top_vector),
            c->irq_top_count, c->irq_last_top_vector, interrupts_name(c->irq_last_top_vector), c->irq_last_top_count);
}

void interrupts_dump_cpus(void)
{
    kprintf("  interrupcoes por CPU: profundidade atual/maxima, ultimos 16 vetores, o mais disparado neste segundo "
            "e no anterior; tempestades mascaradas: %u\n", storm_events);
    for (uint32_t i = 0; i < percpu_count(); i++)
        interrupts_dump_cpu_state(i);
}

void interrupts_backtrace_here(void)
{
    struct thread *t = percpu_ready() ? thread_current() : NULL;
    if (!t || !t->stack)
        return;
    uint64_t rsp = (uint64_t)__builtin_frame_address(0);
    frame_walk(rsp, (uint64_t)t->stack, (uint64_t)t->stack + t->stack_size);
    backtrace(0, rsp & ~7ull, (uint64_t)t->stack, (uint64_t)t->stack + t->stack_size);
}

void interrupts_dump(const struct interrupt_frame *f)
{
    kprintf("  RIP=%016lx CS=%04lx RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx SS=%04lx ERR=%016lx\n", f->rsp, f->ss, f->err_code);
    kprintf("  RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%016lx RDI=%016lx RBP=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf("  CR0=%016lx CR2=%016lx CR3=%016lx CR4=%016lx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());

    struct thread *t = percpu_ready() ? thread_current() : NULL;
    if (!t) {
        backtrace(f->rip, 0, 0, 0);
        return;
    }
    uint64_t base = (uint64_t)t->stack;
    uint64_t top = base + t->stack_size;
    kprintf("  thread tid=%u '%s' cpu=%u, pilha %016lx-%016lx (%s)\n", t->tid, t->name,
            t->cpu, base, top,
            !t->stack ? "pilha de boot"
            : (f->rsp >= base && f->rsp < top) ? "rsp dentro" : "RSP FORA DA PILHA");
    if (t->process)
        kprintf("  processo pid=%u '%s'\n", t->process->pid, t->process->name);
    bool overflow = t->stack && (heap_in_guard(t->stack, read_cr2()) || heap_in_guard(t->stack, f->rsp));
    if (overflow) {
        kprintf("  >>> ESTOURO DA PILHA DO KERNEL: o acesso caiu na pagina de guarda abaixo da pilha <<<\n");
        if (f->rsp < base)
            overflow_frame(f->rsp, base);
    }
    interrupts_dump_cpus();
    if ((f->cs & 3) == 0 && t->stack) {
        frame_walk(f->rbp, base - 0x100, top);
        backtrace(f->rip, f->rsp, base, top);
    }
}

static void handle_exception(struct interrupt_frame *f)
{
    uint64_t cr2 = read_cr2();
    if (handlers[f->int_no].fn) {
        handlers[f->int_no].fn(f, handlers[f->int_no].ctx);
        return;
    }

    const char *guard = NULL;
    if (f->int_no == EXC_PAGE_FAULT && (f->cs & 3) == 0 && cr2 < USER_ADDR_MAX) {
        if (f->err_code & 16)
            guard = "SMEP: o kernel tentou executar codigo de usuario";
        else if (smap_enabled && !(f->rflags & RFLAGS_AC))
            guard = "SMAP: o kernel acessou memoria de usuario fora das copias de uaccess";
    }

    if (!guard && f->int_no == EXC_PAGE_FAULT && cr2 < USER_ADDR_MAX) {
        struct process *p = process_current();
        if (p) {
            bool was_on = (f->rflags & RFLAGS_IF) != 0;
            if (was_on)
                sti();
            int r = vma_fault(p, cr2, f->err_code);
            for (int tries = 0; r == VMA_FAULT_NOMEM && was_on && tries < OOM_FAULT_TRIES; tries++) {
                if (!oom_recover())
                    break;
                r = vma_fault(p, cr2, f->err_code);
            }
            cli();
            if (r == VMA_FAULT_OK)
                return;
            if (r == VMA_FAULT_NOMEM && (f->cs & 3) == 3) {
                if (!oom_fatal_pending()) {
                    log_error_limited("oom", "pid %u '%s': sem memoria para a pagina %016lx; morto", p->pid,
                                      p->name, cr2);
                    signal_send(p, SIGKILL);
                }
                return;
            }
        }
    }

    if (!guard && (f->int_no == EXC_PAGE_FAULT || f->int_no == EXC_GENERAL_PROTECTION) && (f->cs & 3) == 0 &&
        uaccess_fixup(f, cr2))
        return;

    if ((f->cs & 3) == 3 && f->int_no != EXC_DOUBLE_FAULT) {
        struct process *p = process_current();
        int sig = f->int_no == EXC_DIVIDE_ERROR ? SIGFPE
                : f->int_no == EXC_INVALID_OPCODE ? SIGILL : SIGSEGV;
        log_error_limited("proc", "pid %u '%s': %s em RIP=%016lx (err=%lx, CR2=%016lx) -> %s",
                  p->pid, p->name, exception_name((uint8_t)f->int_no), f->rip, f->err_code,
                  cr2, signal_name(sig));
        uint64_t action = SIGACT(p, sig)->sa_handler;
        if (action == SIG_DFL || action == SIG_IGN || (thread_current()->sig_blocked & SIGMASK(sig))) {
            sti();
            process_exit_signal(sig);
        }
        struct sig_src src = { SI_KERNEL, 0, 0, 0, 0 };
        if (sig == SIGSEGV && f->int_no == EXC_PAGE_FAULT) {
            src.code = ((f->err_code & 1) || vma_covers(p, cr2)) ? SEGV_ACCERR : SEGV_MAPERR;
            src.addr = cr2;
        } else if (sig != SIGSEGV) {
            src.code = 1;
            src.addr = f->rip;
        }
        signal_send_thread_info(thread_current(), sig, &src, false);
        return;
    }

    cli();
    smp_halt_others();
    kprint_panic_mode();
    console_set_colors(COLOR_WHITE, COLOR_RED);
    kprintf("\n*** EXCECAO NAO TRATADA: %s (vetor %lu) na CPU %u ***\n",
            exception_name((uint8_t)f->int_no), f->int_no, percpu_cpu_id());

    if (f->int_no == EXC_PAGE_FAULT) {
        uint64_t err = f->err_code;
        kprintf("  Page fault em %016lx: %s, %s, %s%s%s\n", cr2,
                (err & 1) ? "violacao de protecao" : "pagina nao presente",
                (err & 2) ? "escrita" : "leitura",
                (err & 4) ? "ring 3" : "ring 0",
                (err & 8) ? ", bit reservado" : "",
                (err & 16) ? ", busca de instrucao" : "");
    }
    if (guard)
        kprintf("  %s (endereco %016lx); panico controlado.\n", guard, cr2);
    if (f->int_no == EXC_DOUBLE_FAULT)
        kprintf("  Double fault: a CPU nao conseguiu entregar uma excecao (CR2=%016lx).\n"
                "  A causa mais comum e estouro da pilha do kernel.\n", cr2);

    interrupts_dump(f);
    kprintf("Sistema travado.\n");
    halt_forever();
}

static void after_interrupt(struct percpu *pc)
{
    if (pc && pc->irq_depth)
        return;
    irqctx_flush_nmi();
    sched_check_preempt();
}

static bool maskable(uint8_t vec)
{
    if (vec >= VECTOR_IRQ_BASE && vec < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT)
        return vec != VECTOR_IRQ_BASE + IRQ_TIMER;
    return vec >= VECTOR_GSI_BASE && vec < VECTOR_GSI_BASE + VECTOR_GSI_COUNT;
}

static void storm_mask(uint8_t vec, struct percpu *pc, uint32_t count)
{
    storm_masked[vec] = true;
    storm_events++;
    const char *how;
    if (vec >= VECTOR_IRQ_BASE && vec < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT) {
        irq_disable((uint8_t)(vec - VECTOR_IRQ_BASE));
        how = "IRQ mascarada no controlador";
    } else {
        ioapic_set_mask_gsi(VECTOR_IRQ_COUNT + (uint32_t)(vec - VECTOR_GSI_BASE), true);
        how = "GSI mascarada no I/O APIC";
    }
    log_error("irq", "tempestade de interrupcao: vetor %u (%s) disparou %u vezes em menos de 1 s na CPU %u; %s",
              vec, interrupts_name(vec), count, pc->cpu_id, how);
}

static bool note_vector(struct percpu *pc, uint8_t vec)
{
    pc->irq_ring[pc->irq_ring_pos] = vec;
    pc->irq_ring_pos = (uint8_t)((pc->irq_ring_pos + 1) % IRQ_RING);
    uint64_t epoch = time_ticks() / TIME_TICK_HZ;
    if (epoch != pc->irq_rate_epoch) {
        pc->irq_last_top_vector = pc->irq_top_vector;
        pc->irq_last_top_count = pc->irq_top_count;
        pc->irq_top_vector = 0;
        pc->irq_top_count = 0;
        memset(pc->irq_rate, 0, sizeof(pc->irq_rate));
        pc->irq_rate_epoch = epoch;
    }
    uint32_t n = ++pc->irq_rate[vec];
    if (n > peak_rate[vec])
        peak_rate[vec] = n;
    if (n > pc->irq_top_count) {
        pc->irq_top_count = n;
        pc->irq_top_vector = vec;
    }
    if (storm_masked[vec])
        return true;
    if (n > IRQ_STORM_LIMIT && maskable(vec)) {
        storm_mask(vec, pc, n);
        return true;
    }
    return false;
}

void interrupts_check_entry(const struct interrupt_frame *f, const char *how)
{
    struct thread *t = thread_current();
    uint64_t end = (uint64_t)(f + 1);
    if (!t || !t->stack || end == t->kernel_stack_top)
        return;
    struct percpu *pc = percpu_get();
    panic("entrada do anel 3 (%s, vetor %lu) com a pilha errada na CPU %u: quadro termina em %016lx, topo da pilha da "
          "thread '%s' (tid %u) e %016lx (pilha %016lx-%016lx); rsp0 do TSS %016lx, kernel_rsp da CPU %016lx; RIP do "
          "usuario %016lx",
          how, f->int_no, pc->cpu_id, end, t->name, t->tid, t->kernel_stack_top, (uint64_t)t->stack,
          (uint64_t)t->stack + t->stack_size, gdt_kernel_stack(), pc->kernel_rsp, f->rip);
}

uint32_t interrupts_peak_rate(uint8_t vector)
{
    return peak_rate[vector];
}

bool interrupts_storm_masked(uint8_t vector)
{
    return storm_masked[vector];
}

void interrupts_storm_clear(uint8_t vector)
{
    storm_masked[vector] = false;
}

uint32_t interrupts_storm_events(void)
{
    return storm_events;
}

static void check_gs(const struct interrupt_frame *f, uint64_t swapped)
{
    uint64_t vec = f->int_no;
    if (vec == EXC_NMI || vec == EXC_DOUBLE_FAULT || vec == EXC_DEBUG || vec == EXC_MACHINE_CHECK)
        return;
    bool user = (f->cs & 3) == 3;
    if (user == (swapped != 0))
        return;
    if (user)
        panic("entrada do anel 3 (vetor %lu, %s) com o GS do kernel ja carregado: algum caminho voltou ao usuario "
              "sem swapgs; RIP do usuario %016lx", vec, interrupts_name((uint8_t)vec), f->rip);
    const uint8_t *ip = (const uint8_t *)f->rip;
    if (is_text(f->rip) && ip[0] == 0x48 && (ip[1] == 0xCF || (ip[1] == 0x0F && ip[2] == 0x07))) {
        const uint64_t *r = (const uint64_t *)f->rsp;
        if (ip[1] == 0xCF && readable(f->rsp) && readable(f->rsp + 32))
            panic("a volta ao anel 3 falhou no iretq em %016lx: %s (erro %lx) com o GS do usuario ja carregado; "
                  "quadro devolvido: RIP=%016lx CS=%lx RFLAGS=%lx RSP=%016lx SS=%lx",
                  f->rip, interrupts_name((uint8_t)vec), f->err_code, r[0], r[1], r[2], r[3], r[4]);
        panic("a volta ao anel 3 falhou no %s em %016lx: %s (erro %lx) com o GS do usuario ja carregado; RCX=%016lx "
              "R11=%016lx", ip[1] == 0xCF ? "iretq" : "sysretq", f->rip, interrupts_name((uint8_t)vec), f->err_code,
              f->rcx, f->r11);
    }
    panic("o kernel rodava com o GS do usuario: vetor %lu (%s) em RIP=%016lx", vec, interrupts_name((uint8_t)vec),
          f->rip);
}

static uint64_t user_iret_checks;
static uint64_t user_iret_fixed;
static uint32_t user_iret_warned;

void interrupts_check_user_return(struct interrupt_frame *f, const char *where)
{
    if ((f->cs & 3) != 3)
        return;
    __atomic_add_fetch(&user_iret_checks, 1, __ATOMIC_RELAXED);
    uint64_t want_cs = GDT_USER_CODE | GDT_RPL_USER, want_ss = GDT_USER_DATA | GDT_RPL_USER;
    if (f->cs == want_cs && f->ss == want_ss)
        return;
    if (irqctx_strict())
        panic("volta ao anel 3 por iretq (%s) com seletores errados: CS=%lx SS=%lx, esperado CS=%lx SS=%lx; num quadro "
              "de interrupcao o SS e o que o anel 3 tinha, carregado pelo ultimo sysret (STAR) ou iretq; RIP=%016lx",
              where, f->cs, f->ss, want_cs, want_ss, f->rip);
    __atomic_add_fetch(&user_iret_fixed, 1, __ATOMIC_RELAXED);
    if (!__atomic_exchange_n(&user_iret_warned, 1, __ATOMIC_ACQ_REL))
        log_error("irq", "volta ao anel 3 por iretq (%s) com CS=%lx SS=%lx; corrigido para CS=%lx SS=%lx (aviso unico)",
                  where, f->cs, f->ss, want_cs, want_ss);
    f->cs = want_cs;
    f->ss = want_ss;
}

void interrupts_user_iret_stats(uint64_t *checks, uint64_t *fixed)
{
    *checks = __atomic_load_n(&user_iret_checks, __ATOMIC_RELAXED);
    *fixed = __atomic_load_n(&user_iret_fixed, __ATOMIC_RELAXED);
}

static void dispatch(struct interrupt_frame *frame, uint64_t swapped);

void isr_dispatch(struct interrupt_frame *frame, uint64_t swapped)
{
    dispatch(frame, swapped);
    interrupts_check_user_return(frame, "volta de interrupcao ou excecao");
}

static void dispatch(struct interrupt_frame *frame, uint64_t swapped)
{
    check_gs(frame, swapped);
    uint8_t vec = (uint8_t)frame->int_no;
    counters[vec]++;
    struct percpu *pc = percpu_ready() ? percpu_get() : NULL;
    if (pc)
        cpu_counters[pc->cpu_id % MAX_CPUS][vec]++;
    uint8_t saved_vec = 0;
    bool saved_nmi = false;
    bool quiet = false;
    if (pc && (frame->cs & 3) == 3 && vec != EXC_NMI && vec != EXC_DOUBLE_FAULT)
        interrupts_check_entry(frame, "interrupcao");

    if (vec < VECTOR_EXCEPTIONS_END) {
        if (vec == EXC_NMI) {
            if (pc)
                irqctx_enter(pc, vec, true, &saved_vec, &saved_nmi);
            if (!irqctx_nmi_probe() && !prof_nmi(frame))
                handle_exception(frame);
            if (pc)
                irqctx_leave(pc, saved_vec, saved_nmi);
            return;
        }
        if (pc)
            note_vector(pc, vec);
        handle_exception(frame);
        signal_check(frame);
        return;
    }

    if (pc)
        quiet = note_vector(pc, vec);

    if (vec >= VECTOR_IRQ_BASE && vec < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT) {
        uint8_t irq = vec - VECTOR_IRQ_BASE;

        if (irq_mode == IRQ_MODE_PIC && pic_is_spurious(irq))
            return;

        if (pc)
            irqctx_enter(pc, vec, false, &saved_vec, &saved_nmi);
        if (quiet)
            ;
        else if (handlers[vec].fn)
            handlers[vec].fn(frame, handlers[vec].ctx);
        else
            log_warn_limited("irq", "IRQ %u sem handler (vetor %u)", irq, vec);

        if (irq_mode == IRQ_MODE_IOAPIC)
            lapic_eoi();
        else
            pic_send_eoi(irq);
        if (pc)
            irqctx_leave(pc, saved_vec, saved_nmi);
        after_interrupt(pc);
        signal_check(frame);
        return;
    }

    if (vec == VECTOR_SPURIOUS)
        return;

    if (pc)
        irqctx_enter(pc, vec, false, &saved_vec, &saved_nmi);
    if (quiet)
        ;
    else if (handlers[vec].fn)
        handlers[vec].fn(frame, handlers[vec].ctx);
    else
        log_warn_limited("int", "vetor %u sem handler", vec);

    lapic_eoi();
    if (pc)
        irqctx_leave(pc, saved_vec, saved_nmi);
    after_interrupt(pc);
    signal_check(frame);
}

#define STORM_TEST_MSI_FIRES 80000

static volatile uint64_t storm_test_fires;
static int storm_test_vector = -1;

static void storm_test_handler(struct interrupt_frame *f, void *ctx)
{
    (void)f;
    (void)ctx;
    if (++storm_test_fires < STORM_TEST_MSI_FIRES)
        lapic_send_ipi(lapic_id(), (uint8_t)storm_test_vector);
}

int interrupts_storm_selftest(void)
{
    if (!irqctx_strict())
        return -EPERM;
    int vector = interrupts_alloc_vector(storm_test_handler, NULL, "irqstorm");
    if (vector < 0)
        return -1;
    storm_test_vector = vector;
    storm_test_fires = 0;
    uint32_t before = storm_events;
    uint64_t t0 = time_now_us();
    lapic_send_ipi(lapic_id(), (uint8_t)vector);
    uint64_t end = t0 + 5000000;
    while (storm_test_fires < STORM_TEST_MSI_FIRES && time_now_us() < end)
        pause();
    uint64_t msi_ms = (time_now_us() - t0) / 1000;
    uint64_t msi_fires = storm_test_fires;
    uint32_t msi_peak = peak_rate[vector];
    bool msi_masked = storm_masked[vector] || storm_events != before;
    interrupts_unregister((uint8_t)vector);
    names[vector] = NULL;
    peak_rate[vector] = 0;
    storm_test_vector = -1;
    bool msi_ok = msi_fires == STORM_TEST_MSI_FIRES && msi_peak > IRQ_STORM_LIMIT && !msi_masked;
    log_info("irq", "irqstorm: MSI: vetor %d em laco de auto-IPI: %lu disparos em %lu ms, pico de %u por segundo; "
             "mascarado: %s (MSI e MSI-X nunca sao mascarados)", vector, msi_fires, msi_ms, msi_peak,
             msi_masked ? "sim" : "nao");

    uint64_t level_fires = 0, level_ms = 0;
    int level = serial_storm_test(&level_fires, &level_ms);
    if (level == -ENODEV) {
        log_info("irq", "irqstorm: nivel: sem COM1 no I/O APIC, caso de nivel pulado");
    } else {
        log_info("irq", "irqstorm: nivel: COM1 com THRE sem tratamento: %lu disparos ate a mascara em %lu ms; "
                 "IRQ religada e serial de volta: %s", level_fires, level_ms, level == 0 ? "sim" : "nao");
    }
    return msi_ok && level <= 0 ? 0 : -EIO;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));
    memset(counters, 0, sizeof(counters));
    memset(cpu_counters, 0, sizeof(cpu_counters));
    memset(names, 0, sizeof(names));

    for (int i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, ISR_STUB(i), 0, 0);

    idt_set_gate(EXC_BREAKPOINT, ISR_STUB(EXC_BREAKPOINT), 3, 0);
    idt_set_gate(EXC_DOUBLE_FAULT, ISR_STUB(EXC_DOUBLE_FAULT), 0, GDT_IST_DOUBLE_FAULT);
    idt_set_gate(EXC_NMI, ISR_STUB(EXC_NMI), 0, GDT_IST_NMI);

    idt_load();
    log_info("idt", "IDT carregada: 256 vetores em %p; #DF e NMI em pilhas de emergencia; limite de %u "
             "interrupcoes aninhadas e tempestade a partir de %u disparos/s",
             (void *)idt, IRQ_NEST_LIMIT, IRQ_STORM_LIMIT);
}

void idt_load(void)
{
    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}
