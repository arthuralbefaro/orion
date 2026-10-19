#pragma once

#include <kernel/compiler.h>
#include <drivers/nic.h>

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

struct netif {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    bool     configured;
};

struct net_stats {
    uint64_t rx_frames;
    uint64_t rx_arp;
    uint64_t rx_ip;
    uint64_t rx_icmp;
    uint64_t rx_udp;
    uint64_t rx_tcp;
    uint64_t loopback;
    uint64_t loopback_dropped;
    uint64_t rx_unknown;
    uint64_t tx_frames;
    uint64_t checksum_errors;
};

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#define IPV4(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

void net_init(void);
bool net_available(void);
struct netif *net_interface(void);
void net_get_stats(struct net_stats *out);

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);

/* Resolve o MAC de `ip` (ou do gateway), esperando no maximo `timeout_ms`. */
int  net_resolve(uint32_t ip, uint8_t *mac_out, int timeout_ms);

/* Monta e envia um pacote IPv4. `dst` em ordem de host. */
int  net_send_ip(uint32_t dst, uint8_t proto, const void *payload, int len);
int  net_send_udp(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len);
bool net_is_local(uint32_t ip);
uint32_t net_local_for(uint32_t dst);

uint16_t net_checksum(const void *data, int len);
int  net_format_ip(uint32_t ip, char *out, int size);
bool net_parse_ip(const char *s, uint32_t *out);

void net_arp_dump(void);
void net_dump(void);

/* Chamado pela camada de socket para receber pacotes. */
void net_deliver_icmp(uint32_t src, const void *data, int len);
void net_deliver_udp(uint32_t src, uint16_t sport, uint16_t dport, const void *data, int len);


