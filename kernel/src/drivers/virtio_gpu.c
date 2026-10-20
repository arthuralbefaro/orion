#include <drivers/virtio.h>
#include <drivers/virtio_gpu.h>
#include <drivers/console.h>
#include <drivers/fbdev.h>
#include <arch/x86_64/cpu.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/time.h>
#include <lib/string.h>
#include <mm/memory.h>
#include <mm/pmm.h>

#define NO_VECTOR 0xFFFF

#define CMD_GET_DISPLAY_INFO     0x0100
#define CMD_RESOURCE_CREATE_2D   0x0101
#define CMD_RESOURCE_UNREF       0x0102
#define CMD_SET_SCANOUT          0x0103
#define CMD_RESOURCE_FLUSH       0x0104
#define CMD_TRANSFER_TO_HOST_2D  0x0105
#define CMD_RESOURCE_ATTACH_BACKING 0x0106
#define CMD_RESOURCE_DETACH_BACKING 0x0107
#define RESP_OK_NODATA           0x1100
#define RESP_OK_DISPLAY_INFO     0x1101

#define FORMAT_B8G8R8X8 2
#define MAX_SCANOUTS 16
#define FLUSH_MS 33

struct hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence;
    uint32_t ctx;
    uint32_t padding;
} PACKED;

struct rect {
    uint32_t x, y, width, height;
} PACKED;

struct display_info {
    struct hdr hdr;
    struct {
        struct rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[MAX_SCANOUTS];
} PACKED;

struct create_2d {
    struct hdr hdr;
    uint32_t resource;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} PACKED;

struct unref {
    struct hdr hdr;
    uint32_t resource;
    uint32_t padding;
} PACKED;

struct set_scanout {
    struct hdr hdr;
    struct rect r;
    uint32_t scanout;
    uint32_t resource;
} PACKED;

struct flush {
    struct hdr hdr;
    struct rect r;
    uint32_t resource;
    uint32_t padding;
} PACKED;

struct transfer {
    struct hdr hdr;
    struct rect r;
    uint64_t offset;
    uint32_t resource;
    uint32_t padding;
} PACKED;

struct attach {
    struct hdr hdr;
    uint32_t resource;
    uint32_t entries;
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} PACKED;

static struct virtio_dev dev;
static struct virtq q;
static struct mutex lock = MUTEX_INIT("virtio-gpu");
static uint8_t *buf;
static uint64_t buf_phys;
static bool present;
static uint32_t resource;
static uint32_t next_resource = 1;
static uint32_t width, height;
static uint8_t *fb_mem;
static uint64_t fb_phys;
static uint64_t fb_pages;
static uint64_t flushes;
static uint64_t last_gen = ~0ull;

static bool command(const void *req, uint32_t req_len, uint32_t resp_len, uint32_t want)
{
    memcpy(buf, req, req_len);
    memset(buf + 2048, 0, resp_len);
    q.desc[0].addr = buf_phys;
    q.desc[0].len = req_len;
    q.desc[0].flags = VIRTQ_DESC_F_NEXT;
    q.desc[0].next = 1;
    q.desc[1].addr = buf_phys + 2048;
    q.desc[1].len = resp_len;
    q.desc[1].flags = VIRTQ_DESC_F_WRITE;
    q.desc[1].next = 0;
    virtq_push(&q, 0);
    uint64_t end = time_now_us() + 2000000;
    uint16_t head;
    uint32_t len;
    while (!virtq_pop(&q, &head, &len)) {
        if (time_now_us() > end)
            return false;
        pause();
    }
    return ((struct hdr *)(buf + 2048))->type == want;
}

static bool simple(const void *req, uint32_t req_len)
{
    return command(req, req_len, sizeof(struct hdr), RESP_OK_NODATA);
}

static void flush_locked(void)
{
    struct transfer t = { .hdr.type = CMD_TRANSFER_TO_HOST_2D, .r = { 0, 0, width, height }, .resource = resource };
    struct flush f = { .hdr.type = CMD_RESOURCE_FLUSH, .r = { 0, 0, width, height }, .resource = resource };
    if (simple(&t, sizeof(t)) && simple(&f, sizeof(f)))
        flushes++;
}

static int make_scanout(uint32_t w, uint32_t h)
{
    uint64_t pages = PAGE_ALIGN_UP((uint64_t)w * h * 4) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys)
        return -ENOMEM;
    uint8_t *mem = phys_to_virt(phys);
    memset(mem, 0, pages * PAGE_SIZE);
    uint32_t id = next_resource++;
    struct create_2d c = { .hdr.type = CMD_RESOURCE_CREATE_2D, .resource = id, .format = FORMAT_B8G8R8X8,
                           .width = w, .height = h };
    struct attach a = { .hdr.type = CMD_RESOURCE_ATTACH_BACKING, .resource = id, .entries = 1, .addr = phys,
                        .length = (uint32_t)(pages * PAGE_SIZE) };
    struct set_scanout s = { .hdr.type = CMD_SET_SCANOUT, .r = { 0, 0, w, h }, .scanout = 0, .resource = id };
    if (!simple(&c, sizeof(c)) || !simple(&a, sizeof(a)) || !simple(&s, sizeof(s))) {
        pmm_free_pages(phys, pages);
        return -EIO;
    }
    uint32_t old = resource;
    uint64_t old_phys = fb_phys, old_pages = fb_pages;
    resource = id;
    width = w;
    height = h;
    fb_mem = mem;
    fb_phys = phys;
    fb_pages = pages;
    struct framebuffer fb = {
        .address = mem,
        .width = w,
        .height = h,
        .pitch = (uint64_t)w * 4,
        .bpp = 32,
        .red_mask_size = 8,
        .red_mask_shift = 16,
        .green_mask_size = 8,
        .green_mask_shift = 8,
        .blue_mask_size = 8,
        .blue_mask_shift = 0,
    };
    if (console_available())
        console_set_framebuffer(&fb);
    else
        console_init(&fb);
    flush_locked();
    last_gen = console_generation();
    if (old) {
        struct unref u = { .hdr.type = CMD_RESOURCE_UNREF, .resource = old };
        simple(&u, sizeof(u));
        pmm_free_pages(old_phys, old_pages);
    }
    return 0;
}

static void flush_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(FLUSH_MS);
        uint64_t gen = console_generation();
        if (gen == last_gen && !fbdev_graphics_users())
            continue;
        mutex_lock(&lock);
        last_gen = gen;
        flush_locked();
        mutex_unlock(&lock);
    }
}

bool virtio_gpu_init(void)
{
    const struct pci_device *pci = virtio_find(0x1050, 0, 0);
    if (!pci)
        return false;
    if (!virtio_open(&dev, pci, 0))
        return false;
    if (!virtio_queue(&dev, 0, 64, NO_VECTOR, &q)) {
        virtio_fail(&dev);
        return false;
    }
    buf_phys = pmm_alloc_page();
    if (!buf_phys) {
        virtio_fail(&dev);
        return false;
    }
    buf = phys_to_virt(buf_phys);
    virtio_ready(&dev);

    struct hdr h = { .type = CMD_GET_DISPLAY_INFO };
    uint32_t w = 0, hgt = 0;
    if (command(&h, sizeof(h), sizeof(struct display_info), RESP_OK_DISPLAY_INFO)) {
        struct display_info *di = (struct display_info *)(buf + 2048);
        if (di->pmodes[0].enabled) {
            w = di->pmodes[0].r.width;
            hgt = di->pmodes[0].r.height;
        }
    }
    const struct framebuffer *cur = console_get_framebuffer();
    if (cur && cur->width && cur->height) {
        w = (uint32_t)cur->width;
        hgt = (uint32_t)cur->height;
    }
    if (!w || !hgt) {
        w = 1280;
        hgt = 800;
    }
    mutex_lock(&lock);
    int r = make_scanout(w, hgt);
    mutex_unlock(&lock);
    if (r < 0) {
        log_warn("virtio-gpu", "nao consegui criar a tela %ux%u (%d)", w, hgt, r);
        return false;
    }
    present = true;
    thread_create("gpuflush", flush_thread, NULL);
    log_info("virtio-gpu", "virtio-gpu 2D: tela %ux%u no recurso %u, flush a cada %u ms quando muda", w, hgt,
             resource, FLUSH_MS);
    return true;
}

bool virtio_gpu_present(void)
{
    return present;
}

int virtio_gpu_set_mode(uint32_t w, uint32_t h)
{
    if (!present)
        return -ENODEV;
    if (w < 320 || h < 200 || w > 2560 || h > 1600 || w % 8)
        return -EINVAL;
    mutex_lock(&lock);
    int r = make_scanout(w, h);
    mutex_unlock(&lock);
    if (r == 0)
        log_info("virtio-gpu", "modo %ux%u no recurso %u", w, h, resource);
    return r;
}

uint64_t virtio_gpu_flushes(void)
{
    return flushes;
}
