#pragma once

#include <kernel/compiler.h>

struct tcp_rs;

#define TCP_EV_READABLE  1u
#define TCP_EV_WRITABLE  2u
#define TCP_EV_CONNECTED 4u
#define TCP_EV_ERROR     8u
#define TCP_EV_CLOSED    16u

#define TCP_CLOSED      0
#define TCP_SYN_SENT    2
#define TCP_ESTABLISHED 4
#define TCP_FIN_WAIT1   5
#define TCP_FIN_WAIT2   6
#define TCP_CLOSE_WAIT  7
#define TCP_CLOSING     8
#define TCP_LAST_ACK    9
#define TCP_TIME_WAIT   10

#define TCP_SEG_MAX 1600

struct tcp_stats {
    uint64_t rx_segments;
    uint64_t rx_bad;
    uint64_t rx_dropped;
    uint64_t tx_resets;
    uint64_t connections;
};

struct tcp_event {
    uint32_t conn;
    uint32_t events;
};

struct tcp_conn_info {
    uint32_t state;
    int32_t error;
    uint32_t readable;
    uint32_t writable;
    uint32_t unacked;
    uint32_t eof;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
};

struct tcp_rs *tcp_stack_new(void);
int64_t tcp_input(struct tcp_rs *st, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len, uint8_t *out,
                  size_t cap, struct tcp_event *ev);
int64_t tcp_connect(struct tcp_rs *st, uint32_t local_ip, uint32_t remote_ip, uint16_t remote_port, uint32_t iss,
                    uint8_t *out, size_t cap, uint32_t *id);
int64_t tcp_write(struct tcp_rs *st, uint32_t id, const uint8_t *data, size_t len);
int64_t tcp_output(struct tcp_rs *st, uint32_t id, uint8_t *out, size_t cap, uint32_t *dst);
int64_t tcp_read(struct tcp_rs *st, uint32_t id, uint8_t *buf, size_t len);
int tcp_close(struct tcp_rs *st, uint32_t id);
void tcp_detach(struct tcp_rs *st, uint32_t id);
int tcp_info(struct tcp_rs *st, uint32_t id, struct tcp_conn_info *out);
void tcp_stats(const struct tcp_rs *st, struct tcp_stats *out);

void net_tcp_stats(struct tcp_stats *out);
long net_tcp_connect(uint32_t ip, uint16_t port, uint32_t *id);
long net_tcp_write(uint32_t id, const void *buf, size_t len);
long net_tcp_read(uint32_t id, void *buf, size_t len);
int net_tcp_info(uint32_t id, struct tcp_conn_info *out);
void net_tcp_close(uint32_t id);
void socket_tcp_event(uint32_t conn, uint32_t events);
