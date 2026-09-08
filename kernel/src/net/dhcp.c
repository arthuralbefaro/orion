#include <net/net.h>
#include <kernel/log.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <lib/string.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC       0x63825363u

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5

struct dhcp_msg {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} PACKED;

struct dhcp_result {
    uint32_t xid;
    uint32_t offered_ip;
    uint32_t server_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint8_t  type;
    bool     got;
};

static struct dhcp_result pending;
static spinlock_t dhcp_lock = SPINLOCK_INIT;
static struct waitqueue dhcp_wq = WAITQUEUE_INIT;
static bool listening;

static int add_option(uint8_t *opts, int at, uint8_t code, uint8_t len, const void *data)
{
    opts[at++] = code;
    opts[at++] = len;
    memcpy(opts + at, data, len);
    return at + len;
}

static void parse_options(const uint8_t *opts, int len, struct dhcp_result *out)
{
    int i = 0;
    while (i < len) {
        uint8_t code = opts[i++];
        if (code == 0)
            continue;
        if (code == 255 || i >= len)
            break;
        uint8_t olen = opts[i++];
        if (i + olen > len)
            break;
        const uint8_t *v = opts + i;
        uint32_t val = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
        switch (code) {
        case 1:  if (olen >= 4) out->netmask = val; break;
        case 3:  if (olen >= 4) out->gateway = val; break;
        case 6:  if (olen >= 4) out->dns = val; break;
        case 53: if (olen >= 1) out->type = v[0]; break;
        case 54: if (olen >= 4) out->server_ip = val; break;
        default: break;
        }
        i += olen;
    }
}

static void send_msg(uint8_t type, uint32_t xid, uint32_t requested, uint32_t server)
{
    struct dhcp_msg m;
    memset(&m, 0, sizeof(m));
    m.op = 1;
    m.htype = 1;
    m.hlen = ETH_ALEN;
    m.xid = htonl(xid);
    m.flags = htons(0x8000);
    memcpy(m.chaddr, net_interface()->mac, ETH_ALEN);
    m.magic = htonl(DHCP_MAGIC);

    int at = 0;
    at = add_option(m.options, at, 53, 1, &type);
    if (requested) {
        uint32_t be = htonl(requested);
        at = add_option(m.options, at, 50, 4, &be);
    }
    if (server) {
        uint32_t be = htonl(server);
        at = add_option(m.options, at, 54, 4, &be);
    }
    uint8_t params[] = { 1, 3, 6, 15 };
    at = add_option(m.options, at, 55, sizeof(params), params);
    m.options[at++] = 255;

    int total = (int)(sizeof(m) - sizeof(m.options)) + at;
    net_send_udp(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, total);
}

bool dhcp_input(uint32_t src, const void *data, int len)
{
    (void)src;
    if (!listening || len < (int)(sizeof(struct dhcp_msg) - sizeof(((struct dhcp_msg *)0)->options)))
        return false;
    const struct dhcp_msg *m = data;
    if (m->op != 2 || ntohl(m->magic) != DHCP_MAGIC)
        return false;

    uint64_t flags = spin_lock_irqsave(&dhcp_lock);
    if (ntohl(m->xid) != pending.xid) {
        spin_unlock_irqrestore(&dhcp_lock, flags);
        return true;
    }
    pending.offered_ip = ntohl(m->yiaddr);
    int optlen = len - (int)(sizeof(*m) - sizeof(m->options));
    if (optlen > (int)sizeof(m->options))
        optlen = (int)sizeof(m->options);
    parse_options(m->options, optlen, &pending);
    pending.got = true;
    spin_unlock_irqrestore(&dhcp_lock, flags);
    wake_up_all(&dhcp_wq);
    return true;
}

static bool wait_for(uint8_t type, int timeout_ms)
{
    uint64_t end = time_uptime_ms() + (uint64_t)timeout_ms;
    while (time_uptime_ms() < end) {
        uint64_t flags = spin_lock_irqsave(&dhcp_lock);
        bool ok = pending.got && pending.type == type;
        spin_unlock_irqrestore(&dhcp_lock, flags);
        if (ok)
            return true;
        wait_sleep_timeout(&dhcp_wq, NULL, 50);
    }
    return false;
}

int net_dhcp(int timeout_ms)
{
    if (!net_available())
        return -ENODEV;

    uint32_t xid = (uint32_t)time_now_us() ^ 0x4F52494Fu;
    uint64_t flags = spin_lock_irqsave(&dhcp_lock);
    memset(&pending, 0, sizeof(pending));
    pending.xid = xid;
    listening = true;
    spin_unlock_irqrestore(&dhcp_lock, flags);

    net_set_config(0, 0, 0, net_interface()->dns);

    int half = timeout_ms / 2;
    send_msg(DHCPDISCOVER, xid, 0, 0);
    if (!wait_for(DHCPOFFER, half)) {
        listening = false;
        return -ETIMEDOUT;
    }

    flags = spin_lock_irqsave(&dhcp_lock);
    uint32_t offered = pending.offered_ip;
    uint32_t server = pending.server_ip;
    pending.got = false;
    pending.type = 0;
    spin_unlock_irqrestore(&dhcp_lock, flags);

    send_msg(DHCPREQUEST, xid, offered, server);
    if (!wait_for(DHCPACK, half)) {
        listening = false;
        return -ETIMEDOUT;
    }

    flags = spin_lock_irqsave(&dhcp_lock);
    uint32_t ip = pending.offered_ip;
    uint32_t mask = pending.netmask ? pending.netmask : 0xFFFFFF00u;
    uint32_t gw = pending.gateway;
    uint32_t dns = pending.dns;
    listening = false;
    spin_unlock_irqrestore(&dhcp_lock, flags);

    net_set_config(ip, mask, gw, dns);

    char a[16], b[16], c[16], d[16];
    net_format_ip(ip, a, sizeof(a));
    net_format_ip(mask, b, sizeof(b));
    net_format_ip(gw, c, sizeof(c));
    net_format_ip(dns, d, sizeof(d));
    log_info("dhcp", "ip %s mascara %s gateway %s dns %s", a, b, c, d);
    return 0;
}
