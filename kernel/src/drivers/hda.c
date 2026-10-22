#include <drivers/hda.h>
#include <drivers/pci.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <kernel/wait.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/memory.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define INTCTL    0x20
#define INTSTS    0x24
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSTS   0x5D
#define RIRBSIZE  0x5E

#define SD_CTL   0x00
#define SD_STS   0x03
#define SD_LPIB  0x04
#define SD_CBL   0x08
#define SD_LVI   0x0C
#define SD_FMT   0x12
#define SD_BDPL  0x18
#define SD_BDPU  0x1C

#define SD_CTL_SRST (1u << 0)
#define SD_CTL_RUN  (1u << 1)
#define SD_CTL_IOCE (1u << 2)
#define SD_STS_MASK 0x1C

#define CORB_ENTRIES 256
#define RIRB_ENTRIES 256
#define BDL_ENTRIES  8
#define BDL_BYTES    4096
#define RING_BYTES   (BDL_ENTRIES * BDL_BYTES)
#define MAX_NODES    64
#define MAX_PINS     4
#define MAX_PATH     6

#define VERB_GET_PARAM     0xF00
#define VERB_GET_CONN_LIST 0xF02
#define VERB_SET_CONN_SEL  0x701
#define VERB_SET_POWER     0x705
#define VERB_SET_STREAM    0x706
#define VERB_SET_PIN_CTL   0x707
#define VERB_SET_EAPD      0x70C
#define VERB_GET_CONFIG    0xF1C
#define VERB_SET_FORMAT    0x2
#define VERB_SET_AMP       0x3

#define PARAM_VENDOR   0x00
#define PARAM_NODES    0x04
#define PARAM_FUNC     0x05
#define PARAM_WCAP     0x09
#define PARAM_PCM      0x0A
#define PARAM_PINCAP   0x0C
#define PARAM_IN_AMP   0x0D
#define PARAM_CONN_LEN 0x0E
#define PARAM_OUT_AMP  0x12

#define WCAP_TYPE(c)    (((c) >> 20) & 0xF)
#define WCAP_IN_AMP     (1u << 1)
#define WCAP_OUT_AMP    (1u << 2)
#define WCAP_AMP_OVR    (1u << 3)
#define WCAP_CONN_LIST  (1u << 8)
#define WCAP_DIGITAL    (1u << 9)
#define WCAP_POWER      (1u << 10)
#define TYPE_OUTPUT 0
#define TYPE_MIXER  2
#define TYPE_SELECT 3
#define TYPE_PIN    4

struct node {
    uint8_t nid;
    uint32_t caps;
    uint32_t pincap;
    uint32_t config;
    uint8_t nconn;
    uint8_t conn[16];
};

struct pin_path {
    uint8_t pin;
    uint8_t len;
    uint8_t nodes[MAX_PATH];
    uint8_t via[MAX_PATH];
};

static volatile uint8_t *regs;
static uint32_t *corb;
static volatile uint64_t *rirb;
static uint16_t corb_wp;
static uint16_t rirb_rp;
static uint8_t codec;
static uint8_t afg;
static uint32_t vendor_id;
static struct node nodes[MAX_NODES];
static uint32_t nnodes;
static uint8_t dac;
static struct pin_path pins[MAX_PINS];
static uint32_t npins;
static uint32_t rates;
static uint32_t sd;
static uint32_t sd_index;
static uint8_t *ring;
static uint64_t ring_phys;
static uint64_t *bdl;
static uint64_t bdl_phys;
static spinlock_t lock = SPINLOCK_INIT;
static struct waitqueue wq = WAITQUEUE_INIT;
static uint64_t hw_pos, wr_pos, zero_pos;
static uint32_t last_lpib;
static uint64_t underruns, irqs;
static bool running;
static bool draining;
static bool irq_on;
static bool present;
static uint32_t cur_rate, cur_channels;
static int vector = -1;

static inline uint8_t rd8(uint32_t o) { return *(volatile uint8_t *)(regs + o); }
static inline uint16_t rd16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
static inline uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static inline void wr8(uint32_t o, uint8_t v) { *(volatile uint8_t *)(regs + o) = v; }
static inline void wr16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
static inline void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

static bool wait_bits(uint32_t off, uint32_t width, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = time_now_us() + (uint64_t)ms * 1000;
    for (;;) {
        uint32_t v = width == 8 ? rd8(off) : width == 16 ? rd16(off) : rd32(off);
        if ((v & mask) == want)
            return true;
        if (time_now_us() > end)
            return false;
        pause();
    }
}

static bool command(uint8_t nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    uint32_t v = ((uint32_t)codec << 28) | ((uint32_t)nid << 20);
    if (verb <= 0xF)
        v |= (verb << 16) | (payload & 0xFFFF);
    else
        v |= (verb << 8) | (payload & 0xFF);
    corb_wp = (uint16_t)((corb_wp + 1) % CORB_ENTRIES);
    corb[corb_wp] = v;
    __asm__ volatile("" ::: "memory");
    wr16(CORBWP, corb_wp);
    uint64_t end = time_now_us() + 200000;
    while (time_now_us() < end) {
        uint16_t wp = rd16(RIRBWP) & 0xFF;
        if (wp == rirb_rp) {
            pause();
            continue;
        }
        rirb_rp = (uint16_t)((rirb_rp + 1) % RIRB_ENTRIES);
        uint64_t e = rirb[rirb_rp];
        wr8(RIRBSTS, 0x05);
        if ((e >> 32) & 0x10)
            continue;
        if (resp)
            *resp = (uint32_t)e;
        return true;
    }
    return false;
}

static uint32_t param(uint8_t nid, uint32_t p)
{
    uint32_t r = 0;
    if (!command(nid, VERB_GET_PARAM, p, &r))
        return 0;
    return r;
}

static void set(uint8_t nid, uint32_t verb, uint32_t payload)
{
    command(nid, verb, payload, NULL);
}

static struct node *find(uint8_t nid)
{
    for (uint32_t i = 0; i < nnodes; i++)
        if (nodes[i].nid == nid)
            return &nodes[i];
    return NULL;
}

static void read_conns(struct node *n)
{
    n->nconn = 0;
    if (!(n->caps & WCAP_CONN_LIST))
        return;
    uint32_t len = param(n->nid, PARAM_CONN_LEN);
    bool longform = len & 0x80;
    uint32_t count = len & 0x7F;
    uint32_t per = longform ? 2 : 4;
    for (uint32_t i = 0; i < count && n->nconn < 16; i += per) {
        uint32_t r = 0;
        if (!command(n->nid, VERB_GET_CONN_LIST, i, &r))
            return;
        for (uint32_t j = 0; j < per && i + j < count && n->nconn < 16; j++) {
            uint32_t e = longform ? (r >> (16 * j)) & 0xFFFF : (r >> (8 * j)) & 0xFF;
            n->conn[n->nconn++] = (uint8_t)(e & 0xFF);
        }
    }
}

static bool route(uint8_t nid, uint32_t depth, struct pin_path *p)
{
    struct node *n = find(nid);
    if (!n || depth >= MAX_PATH)
        return false;
    for (uint32_t i = 0; i < p->len; i++)
        if (p->nodes[i] == nid)
            return false;
    p->nodes[p->len] = nid;
    p->via[p->len] = 0;
    p->len++;
    uint32_t type = WCAP_TYPE(n->caps);
    if (type == TYPE_OUTPUT && !(n->caps & WCAP_DIGITAL) && (!dac || dac == nid))
        return true;
    if (depth == 0 || type == TYPE_MIXER || type == TYPE_SELECT || type == TYPE_PIN) {
        for (uint32_t i = 0; i < n->nconn; i++) {
            p->via[p->len - 1] = (uint8_t)i;
            if (route(n->conn[i], depth + 1, p))
                return true;
        }
    }
    p->len--;
    return false;
}

static uint32_t amp_caps(struct node *n, bool out)
{
    if (n->caps & WCAP_AMP_OVR)
        return param(n->nid, out ? PARAM_OUT_AMP : PARAM_IN_AMP);
    return param(afg, out ? PARAM_OUT_AMP : PARAM_IN_AMP);
}

static void unmute(struct node *n, bool out, uint32_t index)
{
    uint32_t caps = amp_caps(n, out);
    uint32_t steps = (caps >> 8) & 0x7F;
    uint32_t gain = caps & 0x7F;
    if (gain > steps)
        gain = steps;
    uint32_t payload = (out ? 0x8000u : 0x4000u) | 0x3000u | ((index & 0xF) << 8) | gain;
    set(n->nid, VERB_SET_AMP, payload);
}

static const char *pin_kind(uint32_t config)
{
    switch ((config >> 20) & 0xF) {
    case 0: return "line out";
    case 1: return "alto-falante";
    case 2: return "fone";
    default: return "saida";
    }
}

static void setup_path(struct pin_path *p)
{
    for (uint32_t i = 0; i < p->len; i++) {
        struct node *n = find(p->nodes[i]);
        if (!n)
            continue;
        if (n->caps & WCAP_POWER)
            set(n->nid, VERB_SET_POWER, 0);
        uint32_t type = WCAP_TYPE(n->caps);
        bool last = i + 1 == p->len;
        if (!last && n->nconn > 1 && type != TYPE_MIXER)
            set(n->nid, VERB_SET_CONN_SEL, p->via[i]);
        if (!last && type == TYPE_MIXER && (n->caps & WCAP_IN_AMP))
            unmute(n, false, p->via[i]);
        if (n->caps & WCAP_OUT_AMP)
            unmute(n, true, 0);
        if (type == TYPE_PIN) {
            uint32_t ctl = 0x40;
            if (((n->config >> 20) & 0xF) == 2)
                ctl |= 0x80;
            set(n->nid, VERB_SET_PIN_CTL, ctl);
            if (n->pincap & (1u << 16))
                set(n->nid, VERB_SET_EAPD, 0x02);
        }
    }
}

static bool walk_codec(void)
{
    uint32_t sub = param(0, PARAM_NODES);
    uint32_t start = (sub >> 16) & 0xFF;
    uint32_t count = sub & 0xFF;
    vendor_id = param(0, PARAM_VENDOR);
    afg = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);
        if ((param(nid, PARAM_FUNC) & 0xFF) == 1) {
            afg = nid;
            break;
        }
    }
    if (!afg)
        return false;
    set(afg, VERB_SET_POWER, 0);
    sub = param(afg, PARAM_NODES);
    start = (sub >> 16) & 0xFF;
    count = sub & 0xFF;
    nnodes = 0;
    for (uint32_t i = 0; i < count && nnodes < MAX_NODES; i++) {
        struct node *n = &nodes[nnodes++];
        memset(n, 0, sizeof(*n));
        n->nid = (uint8_t)(start + i);
        n->caps = param(n->nid, PARAM_WCAP);
        if (WCAP_TYPE(n->caps) == TYPE_PIN) {
            n->pincap = param(n->nid, PARAM_PINCAP);
            command(n->nid, VERB_GET_CONFIG, 0, &n->config);
        }
        read_conns(n);
    }
    dac = 0;
    npins = 0;
    for (uint32_t want = 0; want < 3; want++) {
        for (uint32_t i = 0; i < nnodes && npins < MAX_PINS; i++) {
            struct node *n = &nodes[i];
            if (WCAP_TYPE(n->caps) != TYPE_PIN || !(n->pincap & (1u << 4)))
                continue;
            if (((n->config >> 30) & 3) == 1 || ((n->config >> 20) & 0xF) != want)
                continue;
            struct pin_path p;
            memset(&p, 0, sizeof(p));
            p.pin = n->nid;
            if (!route(n->nid, 0, &p))
                continue;
            if (!dac)
                dac = p.nodes[p.len - 1];
            pins[npins++] = p;
        }
    }
    if (!dac)
        return false;
    for (uint32_t i = 0; i < npins; i++)
        setup_path(&pins[i]);
    uint32_t pcm = param(dac, PARAM_PCM);
    if (!pcm)
        pcm = param(afg, PARAM_PCM);
    rates = 0;
    if ((pcm & (1u << 17)) || !(pcm & 0x1F0000)) {
        if (pcm & (1u << 5))
            rates |= HDA_RATE_44100;
        if (pcm & (1u << 6))
            rates |= HDA_RATE_48000;
    }
    if (!rates)
        rates = HDA_RATE_48000;
    return true;
}

static void zero_ring(uint64_t from, uint64_t to)
{
    while (from < to) {
        uint64_t off = from % RING_BYTES;
        uint64_t n = MIN(to - from, RING_BYTES - off);
        memset(ring + off, 0, n);
        from += n;
    }
}

static void advance_locked(void)
{
    if (!running)
        return;
    uint32_t lpib = rd32(sd + SD_LPIB) % RING_BYTES;
    uint32_t delta = (lpib + RING_BYTES - last_lpib) % RING_BYTES;
    last_lpib = lpib;
    hw_pos += delta;
    if (wr_pos < hw_pos) {
        if (!draining)
            underruns++;
        wr_pos = hw_pos;
    }
    uint64_t target = hw_pos + RING_BYTES;
    uint64_t from = MAX(wr_pos, zero_pos);
    if (from < target) {
        zero_ring(from, target);
        zero_pos = target;
    }
}

static void hda_irq(struct interrupt_frame *frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    uint32_t sts = rd32(INTSTS);
    if (!sts)
        return;
    irqs++;
    uint8_t s = rd8(sd + SD_STS);
    if (s & SD_STS_MASK)
        wr8(sd + SD_STS, s & SD_STS_MASK);
    spin_lock(&lock);
    advance_locked();
    spin_unlock(&lock);
    wake_up_all(&wq);
}

static uint16_t format_for(uint32_t rate, uint32_t channels)
{
    uint16_t f = (uint16_t)(1u << 4);
    if (rate == 44100)
        f |= 1u << 14;
    return (uint16_t)(f | ((channels - 1) & 0xF));
}

static void stream_reset(void)
{
    wr8(sd + SD_CTL, rd8(sd + SD_CTL) & ~SD_CTL_RUN);
    wait_bits(sd + SD_CTL, 8, SD_CTL_RUN, 0, 20);
    wr8(sd + SD_CTL, SD_CTL_SRST);
    wait_bits(sd + SD_CTL, 8, SD_CTL_SRST, SD_CTL_SRST, 20);
    wr8(sd + SD_CTL, 0);
    wait_bits(sd + SD_CTL, 8, SD_CTL_SRST, 0, 20);
}

int hda_start(uint32_t rate, uint32_t channels)
{
    if (!present)
        return -ENODEV;
    if ((rate == 48000 && !(rates & HDA_RATE_48000)) || (rate == 44100 && !(rates & HDA_RATE_44100)) ||
        (rate != 48000 && rate != 44100) || channels < 1 || channels > 2)
        return -EINVAL;
    if (running)
        hda_stop(false);
    uint16_t fmt = format_for(rate, channels);
    stream_reset();
    memset(ring, 0, RING_BYTES);
    for (uint32_t i = 0; i < BDL_ENTRIES; i++) {
        bdl[i * 2] = ring_phys + (uint64_t)i * BDL_BYTES;
        bdl[i * 2 + 1] = (uint64_t)BDL_BYTES | (1ull << 32);
    }
    __asm__ volatile("" ::: "memory");
    wr32(sd + SD_BDPL, (uint32_t)bdl_phys);
    wr32(sd + SD_BDPU, (uint32_t)(bdl_phys >> 32));
    wr32(sd + SD_CBL, RING_BYTES);
    wr16(sd + SD_LVI, BDL_ENTRIES - 1);
    wr16(sd + SD_FMT, fmt);
    wr8(sd + SD_CTL + 2, (uint8_t)(1u << 4));
    set(dac, VERB_SET_FORMAT, fmt);
    set(dac, VERB_SET_STREAM, 1u << 4);
    uint64_t flags = spin_lock_irqsave(&lock);
    hw_pos = wr_pos = 0;
    zero_pos = RING_BYTES;
    last_lpib = 0;
    cur_rate = rate;
    cur_channels = channels;
    running = true;
    spin_unlock_irqrestore(&lock, flags);
    wr32(INTCTL, (1u << 31) | (1u << sd_index));
    wr8(sd + SD_STS, SD_STS_MASK);
    wr8(sd + SD_CTL, SD_CTL_RUN | (irq_on ? SD_CTL_IOCE : 0));
    return 0;
}

uint64_t hda_space(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    advance_locked();
    uint64_t space = running ? RING_BYTES - (wr_pos - hw_pos) : 0;
    spin_unlock_irqrestore(&lock, flags);
    return space;
}

int64_t hda_write(const void *buf, uint64_t len, bool nonblock)
{
    const uint8_t *src = buf;
    uint64_t done = 0;
    uint64_t flags = spin_lock_irqsave(&lock);
    while (done < len) {
        if (!running) {
            spin_unlock_irqrestore(&lock, flags);
            return done ? (int64_t)done : -EIO;
        }
        advance_locked();
        uint64_t space = RING_BYTES - (wr_pos - hw_pos);
        if (space == 0) {
            if (nonblock)
                break;
            if (signal_pending_current())
                break;
            wait_sleep_timeout(&wq, &lock, 10);
            continue;
        }
        uint64_t n = MIN(space, len - done);
        uint64_t off = wr_pos % RING_BYTES;
        n = MIN(n, RING_BYTES - off);
        memcpy(ring + off, src + done, n);
        wr_pos += n;
        done += n;
    }
    spin_unlock_irqrestore(&lock, flags);
    if (done == 0 && len > 0)
        return nonblock ? -EAGAIN : -EINTR;
    return (int64_t)done;
}

void hda_stop(bool drain)
{
    if (!present)
        return;
    uint64_t flags = spin_lock_irqsave(&lock);
    if (drain && running) {
        uint64_t limit = time_now_us() + 2000000;
        draining = true;
        advance_locked();
        uint64_t tail = wr_pos + (uint64_t)cur_rate * cur_channels * 2 * 60 / 1000;
        while (running && hw_pos < tail && time_now_us() < limit) {
            wait_sleep_timeout(&wq, &lock, 10);
            advance_locked();
        }
    }
    running = false;
    draining = false;
    spin_unlock_irqrestore(&lock, flags);
    wr8(sd + SD_CTL, 0);
    wait_bits(sd + SD_CTL, 8, SD_CTL_RUN, 0, 20);
    wr32(INTCTL, 0);
    wake_up_all(&wq);
}

static void poll_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(5);
        uint64_t flags = spin_lock_irqsave(&lock);
        advance_locked();
        spin_unlock_irqrestore(&lock, flags);
        wake_up_all(&wq);
    }
}

uint64_t hda_ring_bytes(void)
{
    return RING_BYTES;
}

bool hda_present(void)
{
    return present;
}

uint32_t hda_rates(void)
{
    return rates;
}

size_t hda_report(char *buf, size_t cap)
{
    size_t n = (size_t)ksnprintf(buf, cap, "hda: codec %u, %04x:%04x, AFG %u, DAC %u, %u pino(s), taxas%s%s, %s\n",
                                 codec, vendor_id >> 16, vendor_id & 0xFFFF, afg, dac, npins,
                                 (rates & HDA_RATE_44100) ? " 44100" : "", (rates & HDA_RATE_48000) ? " 48000" : "",
                                 irq_on ? "MSI" : "consulta");
    if (n < cap)
        n += (size_t)ksnprintf(buf + n, cap - n, "hda: %lu interrupcao(oes), %lu falta(s) de dado\n", irqs,
                               underruns);
    return MIN(n, cap);
}

static int16_t sine_table[480];

static void build_sine(uint32_t period, int32_t amp)
{
    int64_t den = period;
    for (uint32_t i = 0; i < period; i++) {
        int64_t num = 2 * (int64_t)i;
        int64_t sign = 1;
        if (num >= den) {
            num -= den;
            sign = -1;
        }
        int64_t q = num * (den - num);
        int64_t v = 16 * q * amp / (5 * den * den - 4 * q);
        sine_table[i] = (int16_t)(sign * v);
    }
}

int hda_tone(uint32_t ms, uint32_t hz)
{
    if (!present)
        return -ENODEV;
    if (hz == 0 || 48000 % hz || 48000 / hz > 480)
        return -EINVAL;
    uint32_t period = 48000 / hz;
    build_sine(period, 12000);
    int r = hda_start(48000, 2);
    if (r < 0)
        return r;
    int16_t chunk[2048];
    uint64_t frames = (uint64_t)ms * 48;
    uint64_t sent = 0;
    uint32_t phase = 0;
    while (sent < frames) {
        uint32_t n = (uint32_t)MIN(frames - sent, 1024);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i * 2] = sine_table[phase];
            chunk[i * 2 + 1] = 0;
            phase = (phase + 1) % period;
        }
        int64_t w = hda_write(chunk, (uint64_t)n * 4, false);
        if (w < 0) {
            hda_stop(false);
            return (int)w;
        }
        sent += n;
    }
    hda_stop(true);
    return 0;
}

bool hda_init(void)
{
    const struct pci_device *dev = pci_find_class(0x04, 0x03, 0);
    if (!dev)
        return false;
    uint64_t base = pci_bar_address(dev, 0);
    if (!base) {
        log_warn("hda", "controlador HDA sem BAR de memoria");
        return false;
    }
    pci_enable_bus_master(dev);
    regs = vmm_map_mmio(base, 0x4000);
    log_info("hda", "controlador HDA %04x:%04x em %02x:%02x.%u", dev->vendor, dev->device, dev->bus, dev->slot,
             dev->func);

    wr32(GCTL, rd32(GCTL) & ~1u);
    if (!wait_bits(GCTL, 32, 1, 0, 100)) {
        log_warn("hda", "o reset do controlador nao baixou");
        return false;
    }
    uint64_t t = time_now_us() + 200;
    while (time_now_us() < t)
        pause();
    wr32(GCTL, rd32(GCTL) | 1u);
    if (!wait_bits(GCTL, 32, 1, 1, 100)) {
        log_warn("hda", "o controlador nao saiu do reset");
        return false;
    }
    t = time_now_us() + 2000;
    while (time_now_us() < t)
        pause();
    uint16_t codecs = rd16(STATESTS);
    if (!codecs) {
        log_warn("hda", "nenhum codec respondeu");
        return false;
    }
    codec = (uint8_t)__builtin_ctz(codecs);
    wr16(STATESTS, codecs);

    uint64_t phys = pmm_alloc_pages(1);
    uint64_t bphys = pmm_alloc_pages(1);
    uint64_t rphys = pmm_alloc_pages(RING_BYTES / PAGE_SIZE);
    if (!phys || !bphys || !rphys) {
        log_warn("hda", "sem memoria para os aneis");
        return false;
    }
    uint8_t *page = phys_to_virt(phys);
    memset(page, 0, PAGE_SIZE);
    corb = (uint32_t *)page;
    rirb = (volatile uint64_t *)(page + 2048);
    bdl = phys_to_virt(bphys);
    bdl_phys = bphys;
    memset(bdl, 0, PAGE_SIZE);
    ring = phys_to_virt(rphys);
    ring_phys = rphys;

    wr8(CORBCTL, 0);
    wr8(RIRBCTL, 0);
    wait_bits(CORBCTL, 8, 2, 0, 20);
    wait_bits(RIRBCTL, 8, 2, 0, 20);
    wr32(CORBLBASE, (uint32_t)phys);
    wr32(CORBUBASE, (uint32_t)(phys >> 32));
    wr8(CORBSIZE, 2);
    wr16(CORBRP, 0x8000);
    wait_bits(CORBRP, 16, 0x8000, 0x8000, 20);
    wr16(CORBRP, 0);
    wait_bits(CORBRP, 16, 0x8000, 0, 20);
    wr16(CORBWP, 0);
    corb_wp = 0;
    wr32(RIRBLBASE, (uint32_t)(phys + 2048));
    wr32(RIRBUBASE, (uint32_t)((phys + 2048) >> 32));
    wr8(RIRBSIZE, 2);
    wr16(RIRBWP, 0x8000);
    rirb_rp = 0;
    wr16(RINTCNT, 0xFF);
    wr8(RIRBSTS, 0x05);
    wr8(CORBCTL, 2);
    wr8(RIRBCTL, 2);

    if (!walk_codec()) {
        log_warn("hda", "codec %u sem caminho de um pino de saida ate um DAC", codec);
        return false;
    }
    uint16_t gcap = rd16(GCAP);
    uint32_t iss = (gcap >> 8) & 0xF;
    uint32_t oss = (gcap >> 12) & 0xF;
    if (!oss) {
        log_warn("hda", "controlador sem stream de saida");
        return false;
    }
    sd_index = iss;
    sd = 0x80 + sd_index * 0x20;

    vector = interrupts_alloc_vector(hda_irq, NULL, "hda");
    uint32_t cpu = irq_next_cpu();
    irq_on = vector >= 0 && pci_enable_msi(dev, (uint8_t)vector, cpu);
    if (!irq_on && vector >= 0)
        interrupts_unregister((uint8_t)vector);
    present = true;
    if (!irq_on)
        thread_create("hdapoll", poll_thread, NULL);
    pci_set_driver(dev, "hda");

    log_info("hda", "codec %u (%04x:%04x): AFG %u, DAC %u, stream de saida %u, %s", codec, vendor_id >> 16,
             vendor_id & 0xFFFF, afg, dac, sd_index, irq_on ? "MSI" : "sem MSI, por consulta");
    for (uint32_t i = 0; i < npins; i++) {
        struct node *n = find(pins[i].pin);
        char path[64];
        size_t len = 0;
        for (uint32_t j = 0; j < pins[i].len && len < sizeof(path); j++)
            len += (size_t)ksnprintf(path + len, sizeof(path) - len, j ? " -> %u" : "%u", pins[i].nodes[j]);
        log_info("hda", "pino %u (%s%s): caminho %s", pins[i].pin, pin_kind(n ? n->config : 0),
                 n && (n->pincap & (1u << 16)) ? ", EAPD" : "", path);
    }
    return true;
}
