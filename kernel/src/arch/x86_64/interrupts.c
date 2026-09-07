#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/cpu.h>
#include <drivers/pic.h>
#include <drivers/ioapic.h>
#include <arch/x86_64/lapic.h>
#include <drivers/console.h>
#include <lib/string.h>
#include <kernel/log.h>

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
#define GATE_TRAP      0xF
#define GATE_PRESENT   (1 << 7)
#define GATE_DPL(n)    ((n) << 5)

static struct idt_entry idt[256] ALIGNED(16);

extern uint8_t isr_stubs_begin[];
#define ISR_STUB_SIZE 16
#define ISR_STUB(vec) ((uint64_t)isr_stubs_begin + (uint64_t)(vec) * ISR_STUB_SIZE)

static void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t dpl)
{
    struct idt_entry *e = &idt[vector];
    e->offset_low  = handler & 0xFFFF;
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = 0;
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

void interrupts_register(uint8_t vector, interrupt_handler_t handler, void *ctx)
{
    uint64_t flags = irq_save();
    handlers[vector].fn = handler;
    handlers[vector].ctx = ctx;
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

static void dump_frame(const struct interrupt_frame *f)
{
    kprintf("  RIP=%016lx CS=%04lx RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx SS=%04lx ERR=%016lx\n", f->rsp, f->ss, f->err_code);
    kprintf("  RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%016lx RDI=%016lx RBP=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    kprintf("  CR0=%016lx CR2=%016lx CR3=%016lx CR4=%016lx\n",
            read_cr0(), read_cr2(), read_cr3(), read_cr4());
}

static void handle_exception(struct interrupt_frame *f)
{
    if (handlers[f->int_no].fn) {
        handlers[f->int_no].fn(f, handlers[f->int_no].ctx);
        return;
    }

    cli();
    console_set_colors(COLOR_WHITE, COLOR_RED);
    kprintf("\n*** EXCECAO NAO TRATADA: %s (vetor %lu) ***\n",
            exception_name((uint8_t)f->int_no), f->int_no);

    if (f->int_no == EXC_PAGE_FAULT) {
        uint64_t err = f->err_code;
        kprintf("  Page fault em %016lx: %s, %s, %s%s%s\n", read_cr2(),
                (err & 1) ? "violacao de protecao" : "pagina nao presente",
                (err & 2) ? "escrita" : "leitura",
                (err & 4) ? "ring 3" : "ring 0",
                (err & 8) ? ", bit reservado" : "",
                (err & 16) ? ", busca de instrucao" : "");
    }

    dump_frame(f);
    kprintf("Sistema travado.\n");
    halt_forever();
}

void isr_dispatch(struct interrupt_frame *frame)
{
    uint8_t vec = (uint8_t)frame->int_no;
    counters[vec]++;

    if (vec < VECTOR_EXCEPTIONS_END) {
        handle_exception(frame);
        return;
    }

    if (vec >= VECTOR_IRQ_BASE && vec < VECTOR_IRQ_BASE + VECTOR_IRQ_COUNT) {
        uint8_t irq = vec - VECTOR_IRQ_BASE;

        /* Modo PIC: IRQ 7/15 podem ser espúrias (ruído / timing do 8259).
         * Nesse caso não há bit no ISR e não se manda EOI. */
        if (irq_mode == IRQ_MODE_PIC && pic_is_spurious(irq))
            return;

        if (handlers[vec].fn)
            handlers[vec].fn(frame, handlers[vec].ctx);
        else
            log_warn("irq", "IRQ %u sem handler (vetor %u)", irq, vec);

        if (irq_mode == IRQ_MODE_IOAPIC)
            lapic_eoi();
        else
            pic_send_eoi(irq);
        return;
    }

    if (vec == VECTOR_SPURIOUS)
        return; /* espúria do LAPIC: ignorar, sem EOI */

    if (handlers[vec].fn)
        handlers[vec].fn(frame, handlers[vec].ctx);
    else
        log_warn("int", "vetor %u sem handler", vec);
        
    lapic_eoi();
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));
    memset(counters, 0, sizeof(counters));

    for (int i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, ISR_STUB(i), 0);

    idt_set_gate(EXC_BREAKPOINT, ISR_STUB(EXC_BREAKPOINT), 3);

    struct idtr idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");

    log_info("idt", "IDT carregada: 256 vetores em %p", (void *)idt);
}