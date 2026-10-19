#pragma once

#include <kernel/compiler.h>

struct percpu;

bool in_interrupt(void);
void irqctx_enter(struct percpu *p, uint8_t vector, bool nmi, uint8_t *saved_vector, bool *saved_nmi);
void irqctx_leave(struct percpu *p, uint8_t saved_vector, bool saved_nmi);
void irqctx_report(const char *what, void *caller);
void irqctx_flush_nmi(void);
bool irqctx_nmi_probe(void);
void irqctx_init(void);
bool irqctx_strict(void);
int irqctx_selftest(void);

#define MIGHT_SLEEP(what)                                                  \
    do {                                                                   \
        if (in_interrupt())                                                \
            irqctx_report((what), __builtin_return_address(0));            \
    } while (0)
