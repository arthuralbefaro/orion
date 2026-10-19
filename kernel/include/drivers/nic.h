#pragma once

#include <kernel/compiler.h>

#define ETH_ALEN      6
#define ETH_MTU       1500
#define ETH_FRAME_MAX 1518

struct waitqueue;

struct nic_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t irqs;
};

struct nic_ops {
    const char *name;
    const uint8_t *(*mac)(void);
    bool (*link_up)(void);
    int (*receive)(void *buf, int max);
    int (*send)(const void *buf, int len);
    void (*stats)(struct nic_stats *out);
    struct waitqueue *(*rx_queue)(void);
};

void nic_register(const struct nic_ops *ops);
bool nic_present(void);
const char *nic_name(void);
const uint8_t *nic_mac(void);
bool nic_link_up(void);
int nic_receive(void *buf, int max);
int nic_send(const void *buf, int len);
void nic_get_stats(struct nic_stats *out);
struct waitqueue *nic_rx_queue(void);
