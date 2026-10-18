#pragma once

#include <kernel/compiler.h>

struct tcp_rs;

struct tcp_stats {
    uint64_t rx_segments;
    uint64_t rx_bad;
    uint64_t rx_dropped;
    uint64_t tx_resets;
    uint64_t connections;
};

struct tcp_rs *tcp_stack_new(void);
int64_t tcp_input(struct tcp_rs *st, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len, uint8_t *out,
                  size_t cap);
void tcp_stats(const struct tcp_rs *st, struct tcp_stats *out);

void net_tcp_stats(struct tcp_stats *out);
