#include <net/net.h>
#include <net/tcp.h>
#include <drivers/e1000.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <lib/string.h>
#include <lib/random.h>
#include <mm/heap.h>

#define ARP_ENTRIES   16
#define ARP_TTL_MS    120000
#define ARP_RETRIES   4

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} PACKED;

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} PACKED;

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} PACKED;

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} PACKED;

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t checksum;
} PACKED;

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint64_t deadline;
    bool     valid;
};

static struct netif netif;
static struct net_stats stats;
static struct arp_entry arp_cache[ARP_ENTRIES];
static spinlock_t arp_lock = SPINLOCK_INIT;
static struct waitqueue arp_wq = WAITQUEUE_INIT;
static const uint8_t broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t ip_id;
static bool running;
static struct tcp_rs *tcp;
static struct mutex tcp_lock = MUTEX_INIT("tcp");
static struct mutex tcp_tx_lock = MUTEX_INIT("tcp-tx");
static uint32_t tcp_loss;
static uint32_t tcp_loss_count;
static uint64_t tcp_lost_rx, tcp_lost_tx;

void net_tcp_set_loss(uint32_t one_in)
{
    __atomic_store_n(&tcp_loss, one_in, __ATOMIC_RELAXED);
}

uint32_t net_tcp_loss(void)
{
    return __atomic_load_n(&tcp_loss, __ATOMIC_RELAXED);
}

static bool tcp_drop(uint64_t *counter)
{
    uint32_t n = net_tcp_loss();
    if (n < 2)
        return false;
    if (__atomic_add_fetch(&tcp_loss_count, 1, __ATOMIC_RELAXED) % n)
        return false;
    __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
    return true;
}

static void tcp_send(uint32_t dst, const uint8_t *seg, int len)
{
    if (!tcp_drop(&tcp_lost_tx))
        net_send_ip(dst, IPPROTO_TCP, seg, len);
}

uint16_t net_checksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

int net_format_ip(uint32_t ip, char *out, int size)
{
    return ksnprintf(out, (size_t)size, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                     (ip >> 8) & 0xFF, ip & 0xFF);
}

bool net_parse_ip(const char *s, uint32_t *out)
{
    uint32_t parts[4] = { 0 };
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return false;
            s++;
        }
        parts[n++] = v;
        if (n < 4) {
            if (*s != '.')
                return false;
            s++;
        }
    }
    if (*s)
        return false;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static int send_frame(const uint8_t *dst_mac, uint16_t type, const void *payload, int len)
{
    uint8_t frame[ETH_FRAME_MAX];
    if (len + (int)sizeof(struct eth_hdr) > ETH_FRAME_MAX)
        return -EINVAL;
    struct eth_hdr *eh = (void *)frame;
    memcpy(eh->dst, dst_mac, ETH_ALEN);
    memcpy(eh->src, netif.mac, ETH_ALEN);
    eh->type = htons(type);
    memcpy(frame + sizeof(*eh), payload, (size_t)len);
    int total = (int)sizeof(*eh) + len;
    if (total < 60)
        total = 60;
    int r = e1000_send(frame, total);
    if (r == 0)
        stats.tx_frames++;
    return r;
}

static void arp_insert(uint32_t ip, const uint8_t *mac)
{
    uint64_t flags = spin_lock_irqsave(&arp_lock);
    struct arp_entry *slot = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = &arp_cache[i];
            break;
        }
        if (!slot && !arp_cache[i].valid)
            slot = &arp_cache[i];
    }
    if (!slot)
        slot = &arp_cache[ip % ARP_ENTRIES];
    slot->ip = ip;
    memcpy(slot->mac, mac, ETH_ALEN);
    slot->deadline = time_uptime_ms() + ARP_TTL_MS;
    slot->valid = true;
    spin_unlock_irqrestore(&arp_lock, flags);
    wake_up_all(&arp_wq);
}

static bool arp_lookup(uint32_t ip, uint8_t *out)
{
    uint64_t flags = spin_lock_irqsave(&arp_lock);
    bool found = false;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            if (time_uptime_ms() > arp_cache[i].deadline) {
                arp_cache[i].valid = false;
                break;
            }
            memcpy(out, arp_cache[i].mac, ETH_ALEN);
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&arp_lock, flags);
    return found;
}

static void arp_request(uint32_t target)
{
    struct arp_hdr a;
    memset(&a, 0, sizeof(a));
    a.htype = htons(1);
    a.ptype = htons(ETH_P_IP);
    a.hlen = ETH_ALEN;
    a.plen = 4;
    a.op = htons(1);
    memcpy(a.sha, netif.mac, ETH_ALEN);
    a.spa = htonl(netif.ip);
    a.tpa = htonl(target);
    send_frame(broadcast_mac, ETH_P_ARP, &a, (int)sizeof(a));
}

static void handle_arp(const uint8_t *data, int len)
{
    if (len < (int)sizeof(struct arp_hdr))
        return;
    const struct arp_hdr *a = (const void *)data;
    if (ntohs(a->ptype) != ETH_P_IP || a->hlen != ETH_ALEN || a->plen != 4)
        return;
    stats.rx_arp++;
    uint32_t spa = ntohl(a->spa);
    uint32_t tpa = ntohl(a->tpa);
    if (spa)
        arp_insert(spa, a->sha);
    if (ntohs(a->op) == 1 && netif.ip && tpa == netif.ip) {
        struct arp_hdr r;
        memset(&r, 0, sizeof(r));
        r.htype = htons(1);
        r.ptype = htons(ETH_P_IP);
        r.hlen = ETH_ALEN;
        r.plen = 4;
        r.op = htons(2);
        memcpy(r.sha, netif.mac, ETH_ALEN);
        r.spa = htonl(netif.ip);
        memcpy(r.tha, a->sha, ETH_ALEN);
        r.tpa = a->spa;
        send_frame(a->sha, ETH_P_ARP, &r, (int)sizeof(r));
    }
}

int net_resolve(uint32_t ip, uint8_t *mac_out, int timeout_ms)
{
    if (ip == 0xFFFFFFFFu || (netif.netmask && (ip | ~netif.netmask) == 0xFFFFFFFFu &&
                              (ip & netif.netmask) == (netif.ip & netif.netmask))) {
        memcpy(mac_out, broadcast_mac, ETH_ALEN);
        return 0;
    }
    uint32_t next = ip;
    if (netif.netmask && (ip & netif.netmask) != (netif.ip & netif.netmask))
        next = netif.gateway;
    if (!next)
        return -ENETUNREACH;

    for (int try = 0; try < ARP_RETRIES; try++) {
        if (arp_lookup(next, mac_out))
            return 0;
        arp_request(next);
        wait_sleep_timeout(&arp_wq, NULL, timeout_ms / ARP_RETRIES + 1);
        if (arp_lookup(next, mac_out))
            return 0;
    }
    return -EHOSTUNREACH;
}

int net_send_ip(uint32_t dst, uint8_t proto, const void *payload, int len)
{
    if (!e1000_present())
        return -ENODEV;
    if (len + (int)sizeof(struct ip_hdr) > ETH_MTU)
        return -EMSGSIZE;

    uint8_t mac[ETH_ALEN];
    int r = net_resolve(dst, mac, 1000);
    if (r < 0)
        return r;

    uint8_t buf[ETH_MTU];
    struct ip_hdr *ih = (void *)buf;
    memset(ih, 0, sizeof(*ih));
    ih->ver_ihl = 0x45;
    ih->total_len = htons((uint16_t)(sizeof(*ih) + len));
    ih->id = htons(ip_id++);
    ih->ttl = 64;
    ih->proto = proto;
    ih->src = htonl(netif.ip);
    ih->dst = htonl(dst);
    ih->checksum = htons(net_checksum(ih, (int)sizeof(*ih)));
    memcpy(buf + sizeof(*ih), payload, (size_t)len);
    return send_frame(mac, ETH_P_IP, buf, (int)sizeof(*ih) + len);
}

static void handle_icmp(uint32_t src, const uint8_t *data, int len)
{
    if (len < (int)sizeof(struct icmp_hdr))
        return;
    stats.rx_icmp++;
    const struct icmp_hdr *ic = (const void *)data;
    if (ic->type == 8) {
        uint8_t reply[ETH_MTU];
        if (len > (int)sizeof(reply))
            return;
        memcpy(reply, data, (size_t)len);
        struct icmp_hdr *r = (void *)reply;
        r->type = 0;
        r->checksum = 0;
        r->checksum = htons(net_checksum(reply, len));
        net_send_ip(src, IPPROTO_ICMP, reply, len);
        return;
    }
    net_deliver_icmp(src, data, len);
}

static void tcp_flush(uint32_t id)
{
    uint8_t *seg = kmalloc(TCP_SEG_MAX);
    if (!seg)
        return;
    mutex_lock(&tcp_tx_lock);
    for (;;) {
        uint32_t dst = 0;
        mutex_lock(&tcp_lock);
        int64_t n = tcp_output(tcp, id, time_uptime_ms(), seg, TCP_SEG_MAX, &dst);
        mutex_unlock(&tcp_lock);
        if (n <= 0)
            break;
        tcp_send(dst, seg, (int)n);
    }
    mutex_unlock(&tcp_tx_lock);
    kfree(seg);
}

static void handle_tcp(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
    stats.rx_tcp++;
    if (!tcp || !netif.configured || dst != netif.ip)
        return;
    if (tcp_drop(&tcp_lost_rx))
        return;
    uint8_t reply[64];
    struct tcp_event ev;
    mutex_lock(&tcp_lock);
    int64_t n = tcp_input(tcp, src, dst, seg, (size_t)len, time_uptime_ms(), reply, sizeof(reply), &ev);
    mutex_unlock(&tcp_lock);
    if (n > 0)
        tcp_send(src, reply, (int)n);
    if (n >= 0 && ev.conn) {
        tcp_flush(ev.conn);
        if (ev.events)
            socket_tcp_event(ev.conn, ev.events);
    }
}

long net_tcp_connect(uint32_t ip, uint16_t port, uint32_t *id)
{
    if (!tcp)
        return -ENODEV;
    if (!netif.configured)
        return -ENETUNREACH;
    uint8_t syn[64];
    mutex_lock(&tcp_lock);
    int64_t n = tcp_connect(tcp, netif.ip, ip, port, (uint32_t)random_u64(), time_uptime_ms(), syn, sizeof(syn), id);
    mutex_unlock(&tcp_lock);
    if (n < 0)
        return n;
    uint8_t mac[ETH_ALEN];
    int r = net_resolve(ip, mac, 1000);
    if (r < 0) {
        net_tcp_close(*id);
        return r;
    }
    tcp_send(ip, syn, (int)n);
    return 0;
}

long net_tcp_write(uint32_t id, const void *buf, size_t len)
{
    mutex_lock(&tcp_lock);
    int64_t n = tcp_write(tcp, id, buf, len);
    mutex_unlock(&tcp_lock);
    if (n > 0)
        tcp_flush(id);
    return n;
}

long net_tcp_read(uint32_t id, void *buf, size_t len)
{
    mutex_lock(&tcp_lock);
    int64_t n = tcp_read(tcp, id, buf, len);
    mutex_unlock(&tcp_lock);
    if (n > 0)
        tcp_flush(id);
    return n;
}

int net_tcp_info(uint32_t id, struct tcp_conn_info *out)
{
    mutex_lock(&tcp_lock);
    int r = tcp_info(tcp, id, out);
    mutex_unlock(&tcp_lock);
    return r;
}

void net_tcp_shutdown(uint32_t id)
{
    mutex_lock(&tcp_lock);
    tcp_close(tcp, id);
    mutex_unlock(&tcp_lock);
    tcp_flush(id);
}

long net_tcp_listen(uint16_t port, uint32_t backlog, bool reuse, uint32_t *id)
{
    if (!tcp)
        return -ENODEV;
    mutex_lock(&tcp_lock);
    int64_t r = tcp_listen(tcp, 0, port, backlog, reuse);
    mutex_unlock(&tcp_lock);
    if (r < 0)
        return r;
    *id = (uint32_t)r;
    return 0;
}

long net_tcp_accept(uint32_t lid)
{
    mutex_lock(&tcp_lock);
    int64_t r = tcp_accept(tcp, lid);
    mutex_unlock(&tcp_lock);
    return r;
}

bool net_tcp_port_busy(uint16_t port, bool reuse)
{
    if (!tcp)
        return false;
    mutex_lock(&tcp_lock);
    bool b = tcp_port_busy(tcp, port, reuse);
    mutex_unlock(&tcp_lock);
    return b;
}

void net_tcp_close(uint32_t id)
{
    mutex_lock(&tcp_lock);
    tcp_close(tcp, id);
    mutex_unlock(&tcp_lock);
    tcp_flush(id);
    mutex_lock(&tcp_lock);
    tcp_detach(tcp, id);
    mutex_unlock(&tcp_lock);
}

void net_tcp_stats(struct tcp_stats *out)
{
    memset(out, 0, sizeof(*out));
    if (!tcp)
        return;
    mutex_lock(&tcp_lock);
    tcp_stats(tcp, out);
    mutex_unlock(&tcp_lock);
}

static void handle_ip(const uint8_t *data, int len)
{
    if (len < (int)sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ih = (const void *)data;
    if ((ih->ver_ihl >> 4) != 4)
        return;
    int ihl = (ih->ver_ihl & 0x0F) * 4;
    if (ihl < (int)sizeof(*ih) || ihl > len)
        return;
    if (net_checksum(ih, ihl) != 0) {
        stats.checksum_errors++;
        return;
    }
    int total = ntohs(ih->total_len);
    if (total > len || total < ihl)
        return;

    stats.rx_ip++;
    uint32_t src = ntohl(ih->src);
    const uint8_t *payload = data + ihl;
    int plen = total - ihl;

    if (ih->proto == IPPROTO_ICMP) {
        handle_icmp(src, payload, plen);
    } else if (ih->proto == IPPROTO_TCP) {
        handle_tcp(src, ntohl(ih->dst), payload, plen);
    } else if (ih->proto == IPPROTO_UDP) {
        if (plen < (int)sizeof(struct udp_hdr))
            return;
        const struct udp_hdr *uh = (const void *)payload;
        int ulen = ntohs(uh->len);
        if (ulen < (int)sizeof(*uh) || ulen > plen)
            return;
        stats.rx_udp++;
        uint16_t dport = ntohs(uh->dport);
        const void *body = payload + sizeof(*uh);
        int blen = ulen - (int)sizeof(*uh);
        if (dport == 68 && dhcp_input(src, body, blen))
            return;
        net_deliver_udp(src, ntohs(uh->sport), dport, body, blen);
    } else {
        stats.rx_unknown++;
    }
}

static void handle_frame(const uint8_t *frame, int len)
{
    if (len < (int)sizeof(struct eth_hdr))
        return;
    stats.rx_frames++;
    const struct eth_hdr *eh = (const void *)frame;
    const uint8_t *payload = frame + sizeof(*eh);
    int plen = len - (int)sizeof(*eh);
    switch (ntohs(eh->type)) {
    case ETH_P_ARP: handle_arp(payload, plen); break;
    case ETH_P_IP:  handle_ip(payload, plen); break;
    default:        stats.rx_unknown++; break;
    }
}

static void rx_thread(void *arg)
{
    (void)arg;
    uint8_t frame[ETH_FRAME_MAX];
    while (running) {
        int len = e1000_receive(frame, (int)sizeof(frame));
        if (len > 0) {
            handle_frame(frame, len);
            continue;
        }
        wait_sleep_timeout(e1000_rx_queue(), NULL, 20);
    }
}

int net_send_udp(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len)
{
    uint8_t buf[ETH_MTU];
    if (len + (int)sizeof(struct udp_hdr) + (int)sizeof(struct ip_hdr) > ETH_MTU)
        return -EMSGSIZE;
    struct udp_hdr *uh = (void *)buf;
    uh->sport = htons(sport);
    uh->dport = htons(dport);
    uh->len = htons((uint16_t)(sizeof(*uh) + len));
    uh->checksum = 0;
    memcpy(buf + sizeof(*uh), data, (size_t)len);
    return net_send_ip(dst, IPPROTO_UDP, buf, (int)sizeof(*uh) + len);
}

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns)
{
    netif.ip = ip;
    netif.netmask = netmask;
    netif.gateway = gateway;
    netif.dns = dns;
    netif.configured = ip != 0;
}

struct netif *net_interface(void)
{
    return &netif;
}

bool net_available(void)
{
    return e1000_present();
}

void net_get_stats(struct net_stats *out)
{
    *out = stats;
}

static void tcp_timer_thread(void *arg)
{
    (void)arg;
    uint8_t *seg = kmalloc(TCP_SEG_MAX);
    if (!seg)
        return;
    for (;;) {
        thread_sleep_ms(50);
        mutex_lock(&tcp_tx_lock);
        for (;;) {
            uint32_t dst = 0;
            struct tcp_event ev;
            mutex_lock(&tcp_lock);
            int64_t n = tcp_tick(tcp, time_uptime_ms(), seg, TCP_SEG_MAX, &dst, &ev);
            mutex_unlock(&tcp_lock);
            if (n > 0)
                tcp_send(dst, seg, (int)n);
            if (ev.conn && ev.events)
                socket_tcp_event(ev.conn, ev.events);
            if (n <= 0 && !ev.conn)
                break;
        }
        mutex_unlock(&tcp_tx_lock);
    }
}

void net_init(void)
{
    if (!e1000_present())
        return;
    memcpy(netif.mac, e1000_mac(), ETH_ALEN);
    tcp = tcp_stack_new(random_u64());
    if (!tcp)
        log_warn("net", "sem memoria para a tabela do TCP");
    running = true;
    if (tcp)
        thread_create("tcptimer", tcp_timer_thread, NULL);
    thread_create("netrx", rx_thread, NULL);
    log_info("net", "pilha IPv4 no ar (ARP, ICMP, UDP, TCP); use 'dhcp' ou 'ifconfig'");
}

void net_arp_dump(void)
{
    char ipbuf[16];
    kprintf("  %-16s %-18s %s\n", "ip", "mac", "expira em");
    uint64_t now = time_uptime_ms();
    int n = 0;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!arp_cache[i].valid)
            continue;
        net_format_ip(arp_cache[i].ip, ipbuf, sizeof(ipbuf));
        kprintf("  %-16s %02x:%02x:%02x:%02x:%02x:%02x %lu s\n", ipbuf,
                arp_cache[i].mac[0], arp_cache[i].mac[1], arp_cache[i].mac[2],
                arp_cache[i].mac[3], arp_cache[i].mac[4], arp_cache[i].mac[5],
                arp_cache[i].deadline > now ? (arp_cache[i].deadline - now) / 1000 : 0);
        n++;
    }
    if (!n)
        kprintf("  (vazia)\n");
}

void net_dump(void)
{
    if (!e1000_present()) {
        kprintf("sem placa de rede\n");
        return;
    }
    char ip[16], mask[16], gw[16], dns[16];
    net_format_ip(netif.ip, ip, sizeof(ip));
    net_format_ip(netif.netmask, mask, sizeof(mask));
    net_format_ip(netif.gateway, gw, sizeof(gw));
    net_format_ip(netif.dns, dns, sizeof(dns));
    kprintf("eth0: %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5],
            e1000_link_up() ? "up" : "down");
    kprintf("  ip %s  mascara %s  gateway %s  dns %s\n", ip, mask, gw, dns);

    struct e1000_stats es;
    e1000_get_stats(&es);
    kprintf("  placa: %lu rx, %lu tx, %lu descartado(s), %lu interrupcao(oes)\n",
            es.rx_packets, es.tx_packets, es.rx_dropped, es.irqs);
    kprintf("  pilha: %lu quadro(s), %lu arp, %lu ip, %lu icmp, %lu udp, %lu ignorado(s)\n",
            stats.rx_frames, stats.rx_arp, stats.rx_ip, stats.rx_icmp, stats.rx_udp,
            stats.rx_unknown);
    struct tcp_stats ts;
    net_tcp_stats(&ts);
    kprintf("  tcp: %lu segmento(s), %lu com erro, %lu descartado(s), %lu RST enviado(s), %lu conexao(oes)\n",
            ts.rx_segments, ts.rx_bad, ts.rx_dropped, ts.tx_resets, ts.connections);
    kprintf("  tcp: %lu retransmissao(oes); perda simulada 1 em %u: %lu recebido(s) e %lu enviado(s) descartado(s)\n",
            ts.retransmits, net_tcp_loss(), tcp_lost_rx, tcp_lost_tx);
}
