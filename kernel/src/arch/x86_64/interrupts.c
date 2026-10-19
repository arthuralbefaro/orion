#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/cpu.h>
#include <drivers/pic.h>
#include <drivers/ioapic.h>
#include <arch/x86_64/lapic.h>
#include <drivers/console.h>
#include <lib/string.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/uaccess.h>
#include <mm/vma.h>
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
    if (vector == VECTOR_LAPIC_TIMER)
        return "lapic-timer";
    if (vector >= VECTOR_IRQ_BASE && vector < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT)
        return "irq-isa";
    if (vector >= VECTOR_GSI_BASE && vector < VECTOR_GSI_BASE + VECTOR_GSI_COUNT)
        return "irq-gsi";
    if (vector >= VECTOR_IPI_HALT && vector <= VECTOR_IPI_TLB_SYNC)
        return "ipi";
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
    if (irq_mode == IRQ_MODE_IOAPIC) {
        ioapic_route_isa(irq, VECTOR_IRQ_BASE + irq, lapic_id());
        ioapic_set_mask_isa(irq, false);
    } else {
        pic_unmask(irq);
    }
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

static void backtrace(uint64_t rip, uint64_t rsp, uint64_t lo, uint64_t hi)
{
    kprintf("  pilha de chamadas (llvm-addr2line -f -e out\\orion.elf <endereco>):\n");
    if (is_text(rip))
        kprintf("    %016lx\n", rip);
    if (rsp < lo || rsp >= hi || (rsp & 7))
        return;
    int shown = 0;
    for (uint64_t *p = (uint64_t *)rsp; (uint64_t)p < hi && shown < 16; p++) {
        if (is_text(*p)) {
            kprintf("    %016lx\n", *p);
            shown++;
        }
    }
}

void interrupts_backtrace_here(void)
{
    struct thread *t = percpu_ready() ? thread_current() : NULL;
    if (!t || !t->stack)
        return;
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
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
    if (t->stack && (heap_in_guard(t->stack, read_cr2()) || heap_in_guard(t->stack, f->rsp)))
        kprintf("  >>> ESTOURO DA PILHA DO KERNEL: o acesso caiu na pagina de guarda abaixo da pilha <<<\n");
    if ((f->cs & 3) == 0 && t->stack)
        backtrace(f->rip, f->rsp, base, top);
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
            bool ok = vma_fault(p, cr2, f->err_code);
            cli();
            if (ok)
                return;
        }
    }

    if (!guard && (f->int_no == EXC_PAGE_FAULT || f->int_no == EXC_GENERAL_PROTECTION) && (f->cs & 3) == 0 &&
        uaccess_fixup(f, cr2))
        return;

    if ((f->cs & 3) == 3 && f->int_no != EXC_DOUBLE_FAULT) {
        struct process *p = process_current();
        int sig = f->int_no == EXC_DIVIDE_ERROR ? SIGFPE
                : f->int_no == EXC_INVALID_OPCODE ? SIGILL : SIGSEGV;
        log_error("proc", "pid %u '%s': %s em RIP=%016lx (err=%lx, CR2=%016lx) -> %s",
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

void isr_dispatch(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    counters[vec]++;
    struct percpu *pc = percpu_ready() ? percpu_get() : NULL;
    if (pc)
        cpu_counters[pc->cpu_id % MAX_CPUS][vec]++;
    uint8_t saved_vec = 0;
    bool saved_nmi = false;

    if (vec < VECTOR_EXCEPTIONS_END) {
        if (vec == EXC_NMI) {
            if (pc)
                irqctx_enter(pc, vec, true, &saved_vec, &saved_nmi);
            if (!irqctx_nmi_probe())
                handle_exception(frame);
            if (pc)
                irqctx_leave(pc, saved_vec, saved_nmi);
            return;
        }
        handle_exception(frame);
        signal_check(frame);
        return;
    }

    if (vec >= VECTOR_IRQ_BASE && vec < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT) {
        uint8_t irq = vec - VECTOR_IRQ_BASE;

        if (irq_mode == IRQ_MODE_PIC && pic_is_spurious(irq))
            return;

        if (pc)
            irqctx_enter(pc, vec, false, &saved_vec, &saved_nmi);
        if (handlers[vec].fn)
            handlers[vec].fn(frame, handlers[vec].ctx);
        else
            log_warn("irq", "IRQ %u sem handler (vetor %u)", irq, vec);

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
    if (handlers[vec].fn)
        handlers[vec].fn(frame, handlers[vec].ctx);
    else
        log_warn("int", "vetor %u sem handler", vec);

    lapic_eoi();
    if (pc)
        irqctx_leave(pc, saved_vec, saved_nmi);
    after_interrupt(pc);
    signal_check(frame);
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
    log_info("idt", "IDT carregada: 256 vetores em %p; #DF e NMI em pilhas de emergencia",
             (void *)idt);
}

void idt_load(void)
{
    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}
