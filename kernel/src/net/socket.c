#include <net/net.h>
#include <net/socket.h>
#include <kernel/file.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/wait.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/uaccess.h>

#define SOCK_QUEUE   8
#define SOCK_MAX_PKT 1500
#define PORT_EPHEMERAL_BASE 49152

struct sock_pkt {
    uint32_t src;
    uint16_t sport;
    uint16_t len;
    uint8_t  data[SOCK_MAX_PKT];
};

struct socket {
    int      type;
    int      proto;
    uint16_t local_port;
    uint32_t peer_ip;
    uint16_t peer_port;
    bool     bound;
    struct sock_pkt *queue[SOCK_QUEUE];
    int      head, count;
    struct waitqueue rx_wq;
    struct socket *next;
};

static struct socket *sockets;
static spinlock_t sock_lock = SPINLOCK_INIT;
static uint16_t next_port = PORT_EPHEMERAL_BASE;

static void queue_push(struct socket *s, uint32_t src, uint16_t sport, const void *data, int len)
{
    if (len > SOCK_MAX_PKT)
        return;
    if (s->count == SOCK_QUEUE) {
        struct sock_pkt *old = s->queue[s->head];
        s->head = (s->head + 1) % SOCK_QUEUE;
        s->count--;
        kfree(old);
    }
    struct sock_pkt *p = kmalloc(sizeof(*p));
    if (!p)
        return;
    p->src = src;
    p->sport = sport;
    p->len = (uint16_t)len;
    memcpy(p->data, data, (size_t)len);
    s->queue[(s->head + s->count) % SOCK_QUEUE] = p;
    s->count++;
}

void net_deliver_udp(uint32_t src, uint16_t sport, uint16_t dport, const void *data, int len)
{
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->type != SOCK_DGRAM || s->local_port != dport)
            continue;
        if (s->peer_ip && (s->peer_ip != src || s->peer_port != sport))
            continue;
        queue_push(s, src, sport, data, len);
        wake_up_all(&s->rx_wq);
        break;
    }
    spin_unlock_irqrestore(&sock_lock, flags);
}

void net_deliver_icmp(uint32_t src, const void *data, int len)
{
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->type != SOCK_RAW || s->proto != IPPROTO_ICMP)
            continue;
        queue_push(s, src, 0, data, len);
        wake_up_all(&s->rx_wq);
    }
    spin_unlock_irqrestore(&sock_lock, flags);
}

static bool port_taken(uint16_t port)
{
    for (struct socket *s = sockets; s; s = s->next)
        if (s->type == SOCK_DGRAM && s->local_port == port)
            return true;
    return false;
}

static void sock_close(struct file *f)
{
    struct socket *s = f->priv;
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    struct socket **pp = &sockets;
    while (*pp && *pp != s)
        pp = &(*pp)->next;
    if (*pp)
        *pp = s->next;
    spin_unlock_irqrestore(&sock_lock, flags);
    for (int i = 0; i < s->count; i++)
        kfree(s->queue[(s->head + i) % SOCK_QUEUE]);
    kfree(s);
}

static short sock_poll(struct file *f, struct poll_table *pt)
{
    struct socket *s = f->priv;
    poll_wait(pt, &s->rx_wq);
    return s->count ? (POLLIN | POLLOUT) : POLLOUT;
}

static long sock_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    (void)f;
    struct ifreq_orion r;
    struct netif *ni = net_interface();
    switch (request) {
    case SIOCGIFCONF_ORION: {
        struct net_stats ns;
        struct e1000_stats es;
        net_get_stats(&ns);
        e1000_get_stats(&es);
        memset(&r, 0, sizeof(r));
        memcpy(r.mac, ni->mac, ETH_ALEN);
        r.ip = ni->ip;
        r.netmask = ni->netmask;
        r.gateway = ni->gateway;
        r.dns = ni->dns;
        r.link_up = e1000_link_up();
        r.rx_packets = es.rx_packets;
        r.tx_packets = es.tx_packets;
        r.rx_dropped = es.rx_dropped;
        r.irqs = es.irqs;
        return copy_to_user((void *)arg, &r, sizeof(r));
    }
    case SIOCSIFCONF_ORION:
        if (copy_from_user(&r, (const void *)arg, sizeof(r)) < 0)
            return -EFAULT;
        net_set_config(r.ip, r.netmask, r.gateway, r.dns);
        return 0;
    case SIOCDHCP_ORION:
        return net_dhcp(5000);
    default:
        return -ENOTTY;
    }
}

static const struct file_ops socket_ops = {
    .close = sock_close,
    .poll = sock_poll,
    .ioctl = sock_ioctl,
};

long sys_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type != SOCK_DGRAM && type != SOCK_RAW)
        return -EPROTONOSUPPORT;
    if (!net_available())
        return -ENODEV;

    struct socket *s = kzalloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->type = type;
    s->proto = type == SOCK_RAW ? (protocol ? protocol : IPPROTO_ICMP) : IPPROTO_UDP;

    struct file *f = file_alloc(&socket_ops, s);
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }

    uint64_t flags = spin_lock_irqsave(&sock_lock);
    if (type == SOCK_DGRAM) {
        for (int i = 0; i < 4096; i++) {
            uint16_t p = next_port++;
            if (next_port == 0)
                next_port = PORT_EPHEMERAL_BASE;
            if (!port_taken(p)) {
                s->local_port = p;
                break;
            }
        }
    }
    s->next = sockets;
    sockets = s;
    spin_unlock_irqrestore(&sock_lock, flags);

    int fd = process_fd_install(process_current(), f);
    if (fd < 0)
        file_unref(f);
    return fd;
}

static struct socket *sock_from_fd(int fd, struct file **fout)
{
    struct file *f = process_fd_pin(fd);
    if (!f || f->ops != &socket_ops)
        return NULL;
    if (fout)
        *fout = f;
    return f->priv;
}

static long sockaddr_in_user(const struct sockaddr_in *uaddr, uint32_t len, struct sockaddr_in *out)
{
    if (!uaddr || len < sizeof(*out))
        return -EINVAL;
    return copy_from_user(out, uaddr, sizeof(*out));
}

long sys_bind(int fd, const struct sockaddr_in *uaddr, uint32_t len)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    struct sockaddr_in a;
    long e = sockaddr_in_user(uaddr, len, &a);
    if (e < 0)
        return e;
    uint16_t port = ntohs(a.sin_port);
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    if (port && port != s->local_port && port_taken(port)) {
        spin_unlock_irqrestore(&sock_lock, flags);
        return -EADDRINUSE;
    }
    if (port)
        s->local_port = port;
    s->bound = true;
    spin_unlock_irqrestore(&sock_lock, flags);
    return 0;
}

long sys_connect(int fd, const struct sockaddr_in *uaddr, uint32_t len)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    struct sockaddr_in a;
    long e = sockaddr_in_user(uaddr, len, &a);
    if (e < 0)
        return e;
    s->peer_ip = ntohl(a.sin_addr);
    s->peer_port = ntohs(a.sin_port);
    return 0;
}

long sys_sendto(int fd, const void *ubuf, uint64_t len, int flags, const struct sockaddr_in *uaddr,
                uint32_t addrlen)
{
    (void)flags;
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (len > SOCK_MAX_PKT)
        return -EMSGSIZE;

    uint32_t dst = s->peer_ip;
    uint16_t dport = s->peer_port;
    if (uaddr && addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in a;
        if (copy_from_user(&a, uaddr, sizeof(a)) < 0)
            return -EFAULT;
        dst = ntohl(a.sin_addr);
        dport = ntohs(a.sin_port);
    }
    if (!dst)
        return -EDESTADDRREQ;

    uint8_t *data = kmalloc(len ? len : 1);
    if (!data)
        return -ENOMEM;
    if (copy_from_user(data, ubuf, len) < 0) {
        kfree(data);
        return -EFAULT;
    }
    int r;
    if (s->type == SOCK_RAW)
        r = net_send_ip(dst, (uint8_t)s->proto, data, (int)len);
    else
        r = net_send_udp(dst, s->local_port, dport, data, (int)len);
    kfree(data);
    return r < 0 ? r : (long)len;
}

long sys_recvfrom(int fd, void *ubuf, uint64_t len, int flags, struct sockaddr_in *uaddr,
                  uint32_t *uaddrlen)
{
    (void)flags;
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    uint32_t addrlen = 0;
    if (uaddr && uaddrlen && copy_from_user(&addrlen, uaddrlen, sizeof(addrlen)) < 0)
        return -EFAULT;

    uint64_t irq = spin_lock_irqsave(&sock_lock);
    while (!s->count) {
        if (f->flags & O_NONBLOCK) {
            spin_unlock_irqrestore(&sock_lock, irq);
            return -EAGAIN;
        }
        if (signal_pending_current()) {
            spin_unlock_irqrestore(&sock_lock, irq);
            return -ERESTARTSYS;
        }
        wait_sleep_timeout(&s->rx_wq, &sock_lock, 200);
    }
    struct sock_pkt *p = s->queue[s->head];
    s->head = (s->head + 1) % SOCK_QUEUE;
    s->count--;
    spin_unlock_irqrestore(&sock_lock, irq);

    uint64_t n = p->len < len ? p->len : len;
    long r = copy_to_user(ubuf, p->data, n);
    if (r == 0 && uaddr && uaddrlen && addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(p->sport);
        a.sin_addr = htonl(p->src);
        addrlen = sizeof(a);
        if (copy_to_user(uaddr, &a, sizeof(a)) < 0 || copy_to_user(uaddrlen, &addrlen, sizeof(addrlen)) < 0)
            r = -EFAULT;
    }
    kfree(p);
    return r < 0 ? r : (long)n;
}
