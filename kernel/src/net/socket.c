#include <net/net.h>
#include <net/socket.h>
#include <net/tcp.h>
#include <kernel/time.h>
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
#define TCP_CONNECT_MS 75000
#define TCP_WAIT_MS    1000
#define STREAM_CHUNK   65536

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
    bool     tcp_open;
    bool     listening;
    bool     reuse;
    bool     shut_rd;
    bool     shut_wr;
    uint32_t tcp_id;
    uint32_t ev_seq;
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

void socket_tcp_event(uint32_t conn, uint32_t events)
{
    (void)events;
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->type != SOCK_STREAM || !s->tcp_open || s->tcp_id != conn)
            continue;
        s->ev_seq++;
        wake_up_all(&s->rx_wq);
        break;
    }
    spin_unlock_irqrestore(&sock_lock, flags);
}

static uint32_t ev_seen(struct socket *s)
{
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    uint32_t v = s->ev_seq;
    spin_unlock_irqrestore(&sock_lock, flags);
    return v;
}

static long ev_wait(struct socket *s, uint32_t seen, uint64_t ms)
{
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    if (s->ev_seq == seen)
        wait_sleep_intr(&s->rx_wq, &sock_lock, ms);
    spin_unlock_irqrestore(&sock_lock, flags);
    return signal_pending_current() ? -ERESTARTSYS : 0;
}

static bool tcp_can_send(const struct tcp_conn_info *in)
{
    return in->state == TCP_ESTABLISHED || in->state == TCP_CLOSE_WAIT;
}

static int64_t stream_read(struct file *f, void *buf, uint64_t len)
{
    struct socket *s = f->priv;
    if (!s->tcp_open || s->listening)
        return -ENOTCONN;
    if (s->shut_rd)
        return 0;
    if (len == 0)
        return 0;
    for (;;) {
        uint32_t seen = ev_seen(s);
        long n = net_tcp_read(s->tcp_id, buf, len);
        if (n != 0)
            return n;
        struct tcp_conn_info in;
        if (net_tcp_info(s->tcp_id, &in) < 0)
            return 0;
        if (in.error)
            return -in.error;
        if (in.eof || in.state == TCP_CLOSED)
            return 0;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        long w = ev_wait(s, seen, TCP_WAIT_MS);
        if (w < 0)
            return w;
    }
}

static int64_t stream_write(struct file *f, const void *buf, uint64_t len)
{
    struct socket *s = f->priv;
    if (!s->tcp_open || s->listening)
        return -ENOTCONN;
    if (s->shut_wr)
        return -EPIPE;
    uint64_t done = 0;
    while (done < len) {
        uint32_t seen = ev_seen(s);
        long n = net_tcp_write(s->tcp_id, (const uint8_t *)buf + done, len - done);
        if (n < 0)
            return done ? (int64_t)done : n;
        done += (uint64_t)n;
        if (n > 0)
            continue;
        if (f->flags & O_NONBLOCK)
            return done ? (int64_t)done : -EAGAIN;
        long w = ev_wait(s, seen, TCP_WAIT_MS);
        if (w < 0)
            return done ? (int64_t)done : w;
    }
    return (int64_t)done;
}

static long stream_connect(struct file *f, struct socket *s, uint32_t ip, uint16_t port)
{
    if (s->tcp_open)
        return -EISCONN;
    uint32_t id = 0;
    long r = net_tcp_connect(ip, port, &id);
    if (r < 0)
        return r;
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    s->tcp_id = id;
    s->tcp_open = true;
    s->peer_ip = ip;
    s->peer_port = port;
    spin_unlock_irqrestore(&sock_lock, flags);
    uint64_t deadline = time_uptime_ms() + TCP_CONNECT_MS;
    for (;;) {
        uint32_t seen = ev_seen(s);
        struct tcp_conn_info in;
        if (net_tcp_info(id, &in) < 0)
            return -ECONNREFUSED;
        s->local_port = in.local_port;
        if (in.error)
            return -in.error;
        if (in.state != TCP_SYN_SENT)
            return in.state == TCP_CLOSED ? -ECONNREFUSED : 0;
        if (f->flags & O_NONBLOCK)
            return -EINPROGRESS;
        uint64_t now = time_uptime_ms();
        if (now >= deadline)
            return -ETIMEDOUT;
        long w = ev_wait(s, seen, deadline - now < TCP_WAIT_MS ? deadline - now : TCP_WAIT_MS);
        if (w < 0)
            return -EINTR;
    }
}

static struct socket *new_socket(int type, int proto)
{
    struct socket *s = kzalloc(sizeof(*s));
    if (s) {
        s->type = type;
        s->proto = proto;
    }
    return s;
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
    if (s->type == SOCK_STREAM && s->tcp_open)
        net_tcp_close(s->tcp_id);
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
    if (s->type == SOCK_STREAM) {
        struct tcp_conn_info in;
        if (!s->tcp_open)
            return POLLOUT | POLLHUP;
        if (net_tcp_info(s->tcp_id, &in) < 0)
            return POLLIN | POLLHUP;
        if (s->listening)
            return in.readable ? POLLIN : 0;
        short ev = 0;
        if (in.readable || in.eof || in.error || in.state == TCP_CLOSED)
            ev |= POLLIN;
        if (tcp_can_send(&in) && in.writable)
            ev |= POLLOUT;
        if (in.error)
            ev |= POLLERR;
        if (in.eof && !tcp_can_send(&in))
            ev |= POLLHUP;
        return ev;
    }
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

static int64_t sock_read(struct file *f, void *buf, uint64_t len)
{
    struct socket *s = f->priv;
    if (s->type != SOCK_STREAM)
        return -EINVAL;
    return stream_read(f, buf, len);
}

static int64_t sock_write(struct file *f, const void *buf, uint64_t len)
{
    struct socket *s = f->priv;
    if (s->type != SOCK_STREAM)
        return -EDESTADDRREQ;
    return stream_write(f, buf, len);
}

static const struct file_ops socket_ops = {
    .read = sock_read,
    .write = sock_write,
    .close = sock_close,
    .poll = sock_poll,
    .ioctl = sock_ioctl,
};

long sys_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    int extra = type & ~SOCK_TYPE_MASK;
    type &= SOCK_TYPE_MASK;
    if (extra & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (type != SOCK_DGRAM && type != SOCK_RAW && type != SOCK_STREAM)
        return -EPROTONOSUPPORT;
    if (type == SOCK_STREAM && protocol && protocol != IPPROTO_TCP)
        return -EPROTONOSUPPORT;
    if (!net_available())
        return -ENODEV;

    struct socket *s = kzalloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->type = type;
    if (type == SOCK_STREAM)
        s->proto = IPPROTO_TCP;
    else
        s->proto = type == SOCK_RAW ? (protocol ? protocol : IPPROTO_ICMP) : IPPROTO_UDP;

    struct file *f = file_alloc(&socket_ops, s);
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }
    if (extra & SOCK_NONBLOCK)
        f->flags |= O_NONBLOCK;

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

    int fd = process_fd_alloc(process_current(), f, 0, (extra & SOCK_CLOEXEC) != 0);
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
    if (s->type == SOCK_STREAM) {
        if (a.sin_family != AF_INET)
            return -EAFNOSUPPORT;
        if (s->bound || s->tcp_open)
            return -EINVAL;
        uint32_t ip = ntohl(a.sin_addr);
        struct netif *ni = net_interface();
        if (ip && ip != ni->ip)
            return -EADDRNOTAVAIL;
        if (port == 0) {
            for (int i = 0; i < 16384 && !port; i++) {
                uint16_t p = next_port++;
                if (next_port == 0)
                    next_port = PORT_EPHEMERAL_BASE;
                if (!net_tcp_port_busy(p, false))
                    port = p;
            }
            if (!port)
                return -EADDRINUSE;
        } else if (net_tcp_port_busy(port, s->reuse)) {
            return -EADDRINUSE;
        }
        s->local_port = port;
        s->bound = true;
        return 0;
    }
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
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    struct sockaddr_in a;
    long e = sockaddr_in_user(uaddr, len, &a);
    if (e < 0)
        return e;
    if (s->type == SOCK_STREAM) {
        if (a.sin_family != AF_INET)
            return -EAFNOSUPPORT;
        return stream_connect(f, s, ntohl(a.sin_addr), ntohs(a.sin_port));
    }
    s->peer_ip = ntohl(a.sin_addr);
    s->peer_port = ntohs(a.sin_port);
    return 0;
}

static long send_kernel(struct file *f, struct socket *s, const uint8_t *data, uint64_t len,
                        const struct sockaddr_in *to)
{
    if (s->type == SOCK_STREAM)
        return stream_write(f, data, len);
    if (len > SOCK_MAX_PKT)
        return -EMSGSIZE;
    uint32_t dst = s->peer_ip;
    uint16_t dport = s->peer_port;
    if (to) {
        dst = ntohl(to->sin_addr);
        dport = ntohs(to->sin_port);
    }
    if (!dst)
        return -EDESTADDRREQ;
    int r;
    if (s->type == SOCK_RAW)
        r = net_send_ip(dst, (uint8_t)s->proto, data, (int)len);
    else
        r = net_send_udp(dst, s->local_port, dport, data, (int)len);
    return r < 0 ? r : (long)len;
}

static long recv_kernel(struct file *f, struct socket *s, uint8_t *buf, uint64_t len, struct sockaddr_in *from,
                        bool *trunc)
{
    memset(from, 0, sizeof(*from));
    *trunc = false;
    if (s->type == SOCK_STREAM)
        return stream_read(f, buf, len);
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
    memcpy(buf, p->data, n);
    *trunc = p->len > len;
    from->sin_family = AF_INET;
    from->sin_port = htons(p->sport);
    from->sin_addr = htonl(p->src);
    kfree(p);
    return (long)n;
}

static uint64_t io_chunk(const struct socket *s, uint64_t len)
{
    uint64_t cap = s->type == SOCK_STREAM ? STREAM_CHUNK : SOCK_MAX_PKT;
    return len < cap ? len : cap;
}

long sys_sendto(int fd, const void *ubuf, uint64_t len, int flags, const struct sockaddr_in *uaddr,
                uint32_t addrlen)
{
    (void)flags;
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    if (!user_range_ok((uint64_t)ubuf, len))
        return -EFAULT;
    if (s->type != SOCK_STREAM && len > SOCK_MAX_PKT)
        return -EMSGSIZE;
    struct sockaddr_in a, *to = NULL;
    if (uaddr && addrlen >= sizeof(a)) {
        if (copy_from_user(&a, uaddr, sizeof(a)) < 0)
            return -EFAULT;
        to = &a;
    }
    uint64_t chunk = io_chunk(s, len);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k)
        return -ENOMEM;
    uint64_t done = 0;
    long r = 0;
    do {
        uint64_t want = len - done < chunk ? len - done : chunk;
        if (copy_from_user(k, (const uint8_t *)ubuf + done, want) < 0) {
            r = -EFAULT;
            break;
        }
        long n = send_kernel(f, s, k, want, to);
        if (n < 0) {
            r = n;
            break;
        }
        done += (uint64_t)n;
        if ((uint64_t)n < want)
            break;
    } while (done < len);
    kfree(k);
    return done ? (long)done : r;
}

static long put_from(const struct sockaddr_in *from, struct sockaddr_in *uaddr, uint32_t *uaddrlen, uint32_t room)
{
    if (!uaddr || !uaddrlen || !from->sin_family)
        return 0;
    uint32_t full = sizeof(*from);
    uint32_t n = room < full ? room : full;
    if (n && copy_to_user(uaddr, from, n) < 0)
        return -EFAULT;
    return copy_to_user(uaddrlen, &full, sizeof(full));
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
    uint32_t room = 0;
    if (uaddr && uaddrlen && copy_from_user(&room, uaddrlen, sizeof(room)) < 0)
        return -EFAULT;
    uint64_t chunk = io_chunk(s, len);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k)
        return -ENOMEM;
    struct sockaddr_in from;
    bool trunc;
    long n = recv_kernel(f, s, k, chunk, &from, &trunc);
    if (n > 0 && copy_to_user(ubuf, k, (size_t)n) < 0)
        n = -EFAULT;
    kfree(k);
    if (n >= 0 && put_from(&from, uaddr, uaddrlen, room) < 0)
        n = -EFAULT;
    return n;
}

struct user_msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t flags;
    int32_t pad2;
};

struct user_iovec {
    uint64_t base;
    uint64_t len;
};

#define MSG_IOV_MAX 1024
#define MSG_TRUNC   0x20

static long msg_load(const struct user_msghdr *um, struct user_msghdr *m, struct user_iovec **iov, uint64_t *total)
{
    if (copy_from_user(m, um, sizeof(*m)) < 0)
        return -EFAULT;
    if (m->iovlen > MSG_IOV_MAX)
        return -EMSGSIZE;
    *iov = kmalloc(m->iovlen ? m->iovlen * sizeof(**iov) : 1);
    if (!*iov)
        return -ENOMEM;
    if (m->iovlen && copy_from_user(*iov, (const void *)m->iov, m->iovlen * sizeof(**iov)) < 0) {
        kfree(*iov);
        return -EFAULT;
    }
    *total = 0;
    for (uint64_t i = 0; i < m->iovlen; i++) {
        long bad = 0;
        if ((*iov)[i].len > (1ull << 31) - *total)
            bad = -EINVAL;
        else if (!user_range_ok((*iov)[i].base, (*iov)[i].len))
            bad = -EFAULT;
        if (bad) {
            kfree(*iov);
            return bad;
        }
        *total += (*iov)[i].len;
    }
    return 0;
}

static long iov_copy(struct user_iovec *iov, uint64_t count, uint64_t *pos, uint8_t *k, uint64_t len, bool to_user)
{
    uint64_t done = 0, skip = *pos;
    for (uint64_t i = 0; i < count && done < len; i++) {
        if (skip >= iov[i].len) {
            skip -= iov[i].len;
            continue;
        }
        uint64_t n = iov[i].len - skip;
        if (n > len - done)
            n = len - done;
        void *u = (uint8_t *)iov[i].base + skip;
        long r = to_user ? copy_to_user(u, k + done, n) : copy_from_user(k + done, u, n);
        if (r < 0)
            return r;
        done += n;
        skip = 0;
    }
    *pos += done;
    return (long)done;
}

long sys_sendmsg(int fd, const struct user_msghdr *umsg, int flags)
{
    (void)flags;
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    struct user_msghdr m;
    struct user_iovec *iov;
    uint64_t total;
    long r = msg_load(umsg, &m, &iov, &total);
    if (r < 0)
        return r;
    struct sockaddr_in a, *to = NULL;
    if (m.name && m.namelen >= sizeof(a)) {
        if (copy_from_user(&a, (const void *)m.name, sizeof(a)) < 0) {
            kfree(iov);
            return -EFAULT;
        }
        to = &a;
    }
    if (s->type != SOCK_STREAM && total > SOCK_MAX_PKT) {
        kfree(iov);
        return -EMSGSIZE;
    }
    uint64_t chunk = io_chunk(s, total);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k) {
        kfree(iov);
        return -ENOMEM;
    }
    uint64_t pos = 0, done = 0;
    r = 0;
    do {
        uint64_t want = total - done < chunk ? total - done : chunk;
        uint64_t at = pos;
        long c = iov_copy(iov, m.iovlen, &at, k, want, false);
        if (c < 0) {
            r = c;
            break;
        }
        long n = send_kernel(f, s, k, want, to);
        if (n < 0) {
            r = n;
            break;
        }
        pos += (uint64_t)n;
        done += (uint64_t)n;
        if ((uint64_t)n < want)
            break;
    } while (done < total);
    kfree(k);
    kfree(iov);
    return done ? (long)done : r;
}

long sys_recvmsg(int fd, struct user_msghdr *umsg, int flags)
{
    (void)flags;
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    struct user_msghdr m;
    struct user_iovec *iov;
    uint64_t total;
    long r = msg_load(umsg, &m, &iov, &total);
    if (r < 0)
        return r;
    uint64_t chunk = io_chunk(s, total);
    uint8_t *k = kmalloc(chunk ? chunk : 1);
    if (!k) {
        kfree(iov);
        return -ENOMEM;
    }
    struct sockaddr_in from;
    bool trunc;
    long n = recv_kernel(f, s, k, chunk, &from, &trunc);
    if (n > 0) {
        uint64_t pos = 0;
        long c = iov_copy(iov, m.iovlen, &pos, k, (uint64_t)n, true);
        if (c < 0)
            n = c;
    }
    kfree(k);
    kfree(iov);
    if (n < 0)
        return n;
    uint32_t namelen = 0;
    if (m.name && from.sin_family) {
        uint32_t room = m.namelen < sizeof(from) ? m.namelen : (uint32_t)sizeof(from);
        if (room && copy_to_user((void *)m.name, &from, room) < 0)
            return -EFAULT;
        namelen = sizeof(from);
    }
    uint64_t controllen = 0;
    int32_t mflags = trunc ? MSG_TRUNC : 0;
    if (copy_to_user(&umsg->namelen, &namelen, sizeof(namelen)) < 0 ||
        copy_to_user(&umsg->controllen, &controllen, sizeof(controllen)) < 0 ||
        copy_to_user(&umsg->flags, &mflags, sizeof(mflags)) < 0)
        return -EFAULT;
    return n;
}

long sys_listen(int fd, int backlog)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->listening)
        return 0;
    if (s->tcp_open)
        return -EINVAL;
    for (int i = 0; i < 16384 && !s->bound; i++) {
        uint16_t p = next_port++;
        if (next_port == 0)
            next_port = PORT_EPHEMERAL_BASE;
        if (!net_tcp_port_busy(p, false)) {
            s->local_port = p;
            s->bound = true;
        }
    }
    if (!s->bound)
        return -EADDRINUSE;
    uint32_t id = 0;
    long r = net_tcp_listen(s->local_port, backlog < 1 ? 1 : (uint32_t)backlog, s->reuse, &id);
    if (r < 0)
        return r;
    uint64_t flags = spin_lock_irqsave(&sock_lock);
    s->tcp_id = id;
    s->tcp_open = true;
    s->listening = true;
    spin_unlock_irqrestore(&sock_lock, flags);
    return 0;
}

static long put_addr(uint32_t ip, uint16_t port, struct sockaddr_in *uaddr, uint32_t *ulen)
{
    if (!uaddr || !ulen)
        return 0;
    uint32_t room;
    if (copy_from_user(&room, ulen, sizeof(room)) < 0)
        return -EFAULT;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr = htonl(ip);
    uint32_t full = sizeof(a);
    uint32_t n = room < full ? room : full;
    if (n && copy_to_user(uaddr, &a, n) < 0)
        return -EFAULT;
    return copy_to_user(ulen, &full, sizeof(full));
}

long sys_accept4(int fd, struct sockaddr_in *uaddr, uint32_t *ulen, int flags)
{
    struct file *f;
    struct socket *s = sock_from_fd(fd, &f);
    if (!s)
        return -ENOTSOCK;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (!s->listening)
        return -EINVAL;
    long child;
    for (;;) {
        uint32_t seen = ev_seen(s);
        child = net_tcp_accept(s->tcp_id);
        if (child >= 0)
            break;
        if (child != -EAGAIN)
            return child;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        long w = ev_wait(s, seen, TCP_WAIT_MS);
        if (w < 0)
            return w;
    }
    struct tcp_conn_info in;
    struct socket *c = new_socket(SOCK_STREAM, IPPROTO_TCP);
    struct file *cf = c ? file_alloc(&socket_ops, c) : NULL;
    if (!c || !cf || net_tcp_info((uint32_t)child, &in) < 0) {
        kfree(c);
        if (cf)
            file_unref(cf);
        net_tcp_close((uint32_t)child);
        return -ENOMEM;
    }
    if (flags & SOCK_NONBLOCK)
        cf->flags |= O_NONBLOCK;
    c->tcp_id = (uint32_t)child;
    c->tcp_open = true;
    c->bound = true;
    c->local_port = in.local_port;
    c->peer_ip = in.remote_ip;
    c->peer_port = in.remote_port;
    uint64_t irq = spin_lock_irqsave(&sock_lock);
    c->next = sockets;
    sockets = c;
    spin_unlock_irqrestore(&sock_lock, irq);
    int nfd = process_fd_alloc(process_current(), cf, 0, (flags & SOCK_CLOEXEC) != 0);
    if (nfd < 0) {
        file_unref(cf);
        return nfd;
    }
    if (put_addr(in.remote_ip, in.remote_port, uaddr, ulen) < 0)
        return -EFAULT;
    return nfd;
}

long sys_shutdown(int fd, int how)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -EINVAL;
    if (s->type != SOCK_STREAM || !s->tcp_open || s->listening)
        return -ENOTCONN;
    if (how != SHUT_WR)
        s->shut_rd = true;
    if (how != SHUT_RD && !s->shut_wr) {
        s->shut_wr = true;
        net_tcp_shutdown(s->tcp_id);
    }
    uint64_t irq = spin_lock_irqsave(&sock_lock);
    s->ev_seq++;
    wake_up_all(&s->rx_wq);
    spin_unlock_irqrestore(&sock_lock, irq);
    return 0;
}

long sys_getsockname(int fd, struct sockaddr_in *uaddr, uint32_t *ulen)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip = 0;
    if (s->type == SOCK_STREAM && s->tcp_open && !s->listening) {
        struct tcp_conn_info in;
        if (net_tcp_info(s->tcp_id, &in) == 0)
            ip = in.local_ip;
    }
    return put_addr(ip, s->local_port, uaddr, ulen);
}

long sys_getpeername(int fd, struct sockaddr_in *uaddr, uint32_t *ulen)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    if (!s->peer_ip || s->listening)
        return -ENOTCONN;
    return put_addr(s->peer_ip, s->peer_port, uaddr, ulen);
}

long sys_setsockopt(int fd, int level, int name, const void *uval, uint32_t len)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    int v = 0;
    if (len < sizeof(int))
        return -EINVAL;
    if (copy_from_user(&v, uval, sizeof(v)) < 0)
        return -EFAULT;
    if (level == SOL_SOCKET && name == SO_REUSEADDR) {
        s->reuse = v != 0;
        return 0;
    }
    if (level == SOL_TCP && name == TCP_NODELAY && s->type == SOCK_STREAM)
        return v ? 0 : -ENOPROTOOPT;
    return -ENOPROTOOPT;
}

long sys_getsockopt(int fd, int level, int name, void *uval, uint32_t *ulen)
{
    struct socket *s = sock_from_fd(fd, NULL);
    if (!s)
        return -ENOTSOCK;
    uint32_t room;
    if (copy_from_user(&room, ulen, sizeof(room)) < 0)
        return -EFAULT;
    if (room < sizeof(int))
        return -EINVAL;
    int v;
    if (level == SOL_SOCKET && name == SO_REUSEADDR) {
        v = s->reuse;
    } else if (level == SOL_SOCKET && name == SO_TYPE) {
        v = s->type;
    } else if (level == SOL_SOCKET && name == SO_ERROR) {
        v = 0;
        if (s->type == SOCK_STREAM && s->tcp_open) {
            struct tcp_conn_info in;
            if (net_tcp_info(s->tcp_id, &in) == 0)
                v = in.error;
        }
    } else if (level == SOL_SOCKET && (name == SO_RCVBUF || name == SO_SNDBUF) && s->type == SOCK_STREAM) {
        v = 32768;
    } else if (level == SOL_TCP && name == TCP_NODELAY && s->type == SOCK_STREAM) {
        v = 1;
    } else {
        return -ENOPROTOOPT;
    }
    uint32_t n = sizeof(v);
    if (copy_to_user(uval, &v, n) < 0 || copy_to_user(ulen, &n, sizeof(n)) < 0)
        return -EFAULT;
    return 0;
}
