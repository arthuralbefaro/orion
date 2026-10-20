#include <drivers/dsp.h>
#include <drivers/hda.h>
#include <kernel/file.h>
#include <kernel/syscall.h>
#include <mm/heap.h>
#include <mm/uaccess.h>
#include <lib/string.h>

#define AFMT_QUERY  0x00
#define AFMT_U8     0x08
#define AFMT_S16_LE 0x10

#define SNDCTL_DSP_RESET      0x00005000u
#define SNDCTL_DSP_SYNC       0x00005001u
#define SNDCTL_DSP_SPEED      0xC0045002u
#define SNDCTL_DSP_STEREO     0xC0045003u
#define SNDCTL_DSP_GETBLKSIZE 0xC0045004u
#define SNDCTL_DSP_SETFMT     0xC0045005u
#define SNDCTL_DSP_CHANNELS   0xC0045006u
#define SNDCTL_DSP_GETFMTS    0x8004500Bu
#define SNDCTL_DSP_GETOSPACE  0x8010500Cu

#define FRAG_BYTES   4096
#define STAGE_FRAMES 1024
#define ONE          (1ull << 32)

struct audio_buf_info {
    int32_t fragments;
    int32_t fragstotal;
    int32_t fragsize;
    int32_t bytes;
};

struct dsp {
    uint32_t rate;
    uint32_t channels;
    uint32_t fmt;
    uint32_t out_rate;
    bool started;
    bool primed;
    uint64_t step;
    uint64_t pos;
    int32_t prev[2];
    int32_t cur[2];
    uint32_t staged;
    int16_t stage[STAGE_FRAMES * 2];
    uint8_t in[4096];
};

static bool busy;

static uint32_t frame_bytes(const struct dsp *d)
{
    return d->channels * (d->fmt == AFMT_S16_LE ? 2 : 1);
}

static void reset_converter(struct dsp *d)
{
    d->primed = false;
    d->pos = 0;
    d->staged = 0;
}

static void stop(struct dsp *d, bool drain)
{
    if (d->started)
        hda_stop(drain);
    d->started = false;
    reset_converter(d);
}

static int start(struct dsp *d)
{
    uint32_t rates = hda_rates();
    if (d->rate == 44100 && (rates & HDA_RATE_44100))
        d->out_rate = 44100;
    else if (rates & HDA_RATE_48000)
        d->out_rate = 48000;
    else
        d->out_rate = 44100;
    d->step = ((uint64_t)d->rate << 32) / d->out_rate;
    reset_converter(d);
    int r = hda_start(d->out_rate, 2);
    if (r < 0)
        return r;
    d->started = true;
    return 0;
}

static int32_t sample_at(const struct dsp *d, const uint8_t *p, uint32_t ch)
{
    if (d->fmt == AFMT_S16_LE)
        return (int16_t)(p[ch * 2] | (p[ch * 2 + 1] << 8));
    return ((int32_t)p[ch] - 128) << 8;
}

static int64_t flush_stage(struct dsp *d, bool nonblock)
{
    if (!d->staged)
        return 0;
    int64_t r = hda_write(d->stage, (uint64_t)d->staged * 4, nonblock);
    d->staged = 0;
    return r < 0 ? r : 0;
}

static void feed(struct dsp *d, const uint8_t *frame)
{
    int32_t l = sample_at(d, frame, 0);
    int32_t r = d->channels > 1 ? sample_at(d, frame, 1) : l;
    if (!d->primed) {
        d->cur[0] = l;
        d->cur[1] = r;
        d->primed = true;
        return;
    }
    d->prev[0] = d->cur[0];
    d->prev[1] = d->cur[1];
    d->cur[0] = l;
    d->cur[1] = r;
    while (d->pos < ONE) {
        int64_t f = (int64_t)(d->pos >> 16);
        for (int c = 0; c < 2; c++) {
            int64_t v = d->prev[c] + (((int64_t)(d->cur[c] - d->prev[c]) * f) >> 16);
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            d->stage[d->staged * 2 + (uint32_t)c] = (int16_t)v;
        }
        d->staged++;
        d->pos += d->step;
    }
    d->pos -= ONE;
}

static int64_t dsp_write(struct file *f, const void *buf, uint64_t len)
{
    struct dsp *d = f->priv;
    bool nonblock = (f->flags & O_NONBLOCK) != 0;
    uint32_t fb = frame_bytes(d);
    if (!d->started) {
        int r = start(d);
        if (r < 0)
            return r;
    }
    uint64_t done = 0;
    uint32_t chunk_frames = (uint32_t)(((uint64_t)(STAGE_FRAMES - 2) * d->rate) / d->out_rate);
    if (chunk_frames == 0)
        chunk_frames = 1;
    if (chunk_frames > sizeof(d->in) / fb)
        chunk_frames = sizeof(d->in) / fb;
    while (len - done >= fb) {
        uint32_t frames = (uint32_t)MIN((len - done) / fb, chunk_frames);
        if (nonblock) {
            uint64_t space = hda_space() / 4;
            uint64_t fit = space > 2 ? ((space - 2) * d->rate) / d->out_rate : 0;
            if (fit == 0)
                break;
            frames = (uint32_t)MIN(frames, fit);
        }
        memcpy(d->in, (const uint8_t *)buf + done, (size_t)frames * fb);
        for (uint32_t i = 0; i < frames; i++)
            feed(d, d->in + i * fb);
        int64_t r = flush_stage(d, nonblock);
        done += (uint64_t)frames * fb;
        if (r < 0)
            return r == -EINTR && done ? (int64_t)done : r;
    }
    if (done == 0 && len >= fb)
        return nonblock ? -EAGAIN : -EIO;
    return (int64_t)(done ? done : len);
}

static int get_int(uint64_t arg, int32_t *v)
{
    return copy_from_user(v, (const void *)arg, sizeof(*v)) != 0 ? -EFAULT : 0;
}

static int put_int(uint64_t arg, int32_t v)
{
    return copy_to_user((void *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
}

static long dsp_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    struct dsp *d = f->priv;
    int32_t v = 0;
    switch ((uint32_t)request) {
    case SNDCTL_DSP_RESET:
        stop(d, false);
        return 0;
    case SNDCTL_DSP_SYNC:
        stop(d, true);
        return 0;
    case SNDCTL_DSP_SPEED:
        if (get_int(arg, &v) < 0)
            return -EFAULT;
        if (v < 4000)
            v = 4000;
        if (v > 96000)
            v = 96000;
        if ((uint32_t)v != d->rate) {
            stop(d, true);
            d->rate = (uint32_t)v;
        }
        return put_int(arg, (int32_t)d->rate);
    case SNDCTL_DSP_STEREO:
        if (get_int(arg, &v) < 0)
            return -EFAULT;
        stop(d, true);
        d->channels = v ? 2 : 1;
        return put_int(arg, d->channels == 2);
    case SNDCTL_DSP_CHANNELS:
        if (get_int(arg, &v) < 0)
            return -EFAULT;
        stop(d, true);
        d->channels = v == 1 ? 1 : 2;
        return put_int(arg, (int32_t)d->channels);
    case SNDCTL_DSP_SETFMT:
        if (get_int(arg, &v) < 0)
            return -EFAULT;
        if (v != AFMT_QUERY) {
            stop(d, true);
            d->fmt = v == AFMT_U8 ? AFMT_U8 : AFMT_S16_LE;
        }
        return put_int(arg, (int32_t)d->fmt);
    case SNDCTL_DSP_GETFMTS:
        return put_int(arg, AFMT_U8 | AFMT_S16_LE);
    case SNDCTL_DSP_GETBLKSIZE:
        return put_int(arg, FRAG_BYTES);
    case SNDCTL_DSP_GETOSPACE: {
        uint64_t out_frames = (d->started ? hda_space() : hda_ring_bytes()) / 4;
        uint32_t out_rate = d->started ? d->out_rate : 48000;
        uint64_t in_bytes = out_frames * d->rate / out_rate * frame_bytes(d);
        uint64_t total = hda_ring_bytes() / 4 * d->rate / out_rate * frame_bytes(d);
        struct audio_buf_info info = {
            .fragments = (int32_t)(in_bytes / FRAG_BYTES),
            .fragstotal = (int32_t)(total / FRAG_BYTES),
            .fragsize = FRAG_BYTES,
            .bytes = (int32_t)in_bytes,
        };
        return copy_to_user((void *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

static void dsp_close(struct file *f)
{
    struct dsp *d = f->priv;
    stop(d, true);
    kfree(d);
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
}

static const struct file_ops dsp_ops = {
    .write = dsp_write,
    .ioctl = dsp_ioctl,
    .close = dsp_close,
};

static int open_error;

struct file *dsp_open(void)
{
    if (!hda_present()) {
        open_error = -ENODEV;
        return NULL;
    }
    bool expected = false;
    if (!__atomic_compare_exchange_n(&busy, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        open_error = -EBUSY;
        return NULL;
    }
    struct dsp *d = kzalloc(sizeof(*d));
    if (!d) {
        __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
        open_error = -ENOMEM;
        return NULL;
    }
    d->rate = 8000;
    d->channels = 1;
    d->fmt = AFMT_U8;
    struct file *f = file_alloc(&dsp_ops, d);
    if (!f) {
        kfree(d);
        __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
        open_error = -ENOMEM;
        return NULL;
    }
    f->mode = S_IFCHR | 0666;
    return f;
}

int dsp_open_error(void)
{
    return open_error;
}
