#pragma once

#include <kernel/compiler.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

#define LDISC_LINE_MAX  256
#define LDISC_READY_MAX 1024

typedef void (*ldisc_echo_fn)(void *ctx, const char *data, uint64_t len);

struct line_discipline {
    spinlock_t lock;
    struct waitqueue readers;
    char line[LDISC_LINE_MAX];
    uint32_t line_len;
    char ready[LDISC_READY_MAX];
    uint32_t head;
    uint32_t tail;
    bool eof;
    bool hangup;
    ldisc_echo_fn echo;
    void *ctx;
};

void ldisc_init(struct line_discipline *l, ldisc_echo_fn echo, void *ctx);
void ldisc_input(struct line_discipline *l, char c);
void ldisc_write(struct line_discipline *l, const void *buf, uint64_t len);
int64_t ldisc_read(struct line_discipline *l, void *buf, uint64_t len);
uint32_t ldisc_available(struct line_discipline *l);
short ldisc_poll(struct line_discipline *l, struct poll_table *pt);
void ldisc_hangup(struct line_discipline *l);
