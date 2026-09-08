#pragma once

#include <kernel/compiler.h>

#define ETH_ALEN     6
#define ETH_MTU      1500
#define ETH_FRAME_MAX 1518

struct e1000_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t irqs;
};

bool e1000_init(void);
bool e1000_present(void);
const uint8_t *e1000_mac(void);
bool e1000_link_up(void);

/* Copia o proximo quadro recebido para `buf`. Devolve o tamanho ou 0. */
int  e1000_receive(void *buf, int max);

/* Enfileira um quadro para transmissao. Devolve 0 ou -errno. */
int  e1000_send(const void *buf, int len);

void e1000_get_stats(struct e1000_stats *out);

struct waitqueue;
struct waitqueue *e1000_rx_queue(void);
