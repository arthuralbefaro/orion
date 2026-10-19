#include <drivers/nic.h>
#include <kernel/syscall.h>
#include <kernel/log.h>
#include <kernel/wait.h>
#include <lib/string.h>

static const struct nic_ops *ops;
static struct waitqueue idle_wq = WAITQUEUE_INIT;
static const uint8_t zero_mac[ETH_ALEN];

void nic_register(const struct nic_ops *o)
{
    if (ops) {
        log_warn("nic", "%s ignorada: a rede ja usa %s", o->name, ops->name);
        return;
    }
    ops = o;
}

bool nic_present(void)
{
    return ops != NULL;
}

const char *nic_name(void)
{
    return ops ? ops->name : "nenhuma";
}

const uint8_t *nic_mac(void)
{
    return ops ? ops->mac() : zero_mac;
}

bool nic_link_up(void)
{
    return ops && ops->link_up();
}

int nic_receive(void *buf, int max)
{
    return ops ? ops->receive(buf, max) : 0;
}

int nic_send(const void *buf, int len)
{
    return ops ? ops->send(buf, len) : -ENODEV;
}

void nic_get_stats(struct nic_stats *out)
{
    if (ops)
        ops->stats(out);
    else
        memset(out, 0, sizeof(*out));
}

struct waitqueue *nic_rx_queue(void)
{
    return ops ? ops->rx_queue() : &idle_wq;
}
