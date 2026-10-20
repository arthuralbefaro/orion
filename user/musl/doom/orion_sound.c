#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "orion_sound.h"

#define NUM_CHANNELS 8
#define MIX_RATE     11025
#define MIX_FRAMES   1024
#define TARGET_MS    140

struct sample {
    const uint8_t *pcm;
    uint32_t len;
    uint32_t step;
};

struct channel {
    const struct sample *s;
    uint32_t pos;
    int left;
    int right;
};

static int dsp = -1;
static boolean use_prefix;
static struct channel channels[NUM_CHANNELS];
static int16_t mixbuf[MIX_FRAMES * 2];
static unsigned long started, writes, eagain, mixed;

int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *timidity_cfg_path = "";
char *gus_patch_path = "";
int gus_ram_kb = 1024;

void I_InitTimidityConfig(void)
{
}

static struct sample *load(sfxinfo_t *sfx)
{
    if (sfx->driver_data)
        return sfx->driver_data;
    int lump = sfx->lumpnum;
    if (lump < 0)
        return NULL;
    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
    int lumplen = W_LumpLength(lump);
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
        return NULL;
    uint32_t rate = (uint32_t)(data[2] | (data[3] << 8));
    uint32_t length = (uint32_t)(data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24));
    if (length > (uint32_t)lumplen - 8 || length <= 48 || rate == 0)
        return NULL;
    struct sample *s = malloc(sizeof(*s));
    if (!s)
        return NULL;
    s->pcm = data + 8 + 16;
    s->len = length - 32;
    s->step = (uint32_t)(((uint64_t)rate << 16) / MIX_RATE);
    sfx->driver_data = s;
    return s;
}

static void set_params(struct channel *c, int vol, int sep)
{
    if (vol < 0)
        vol = 0;
    if (vol > 127)
        vol = 127;
    if (sep < 0)
        sep = 0;
    if (sep > 254)
        sep = 254;
    c->left = vol * (254 - sep) / 254;
    c->right = vol * sep / 254;
}

static void mix(uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (int k = 0; k < NUM_CHANNELS; k++) {
            struct channel *c = &channels[k];
            if (!c->s)
                continue;
            uint32_t idx = c->pos >> 16;
            if (idx >= c->s->len) {
                c->s = NULL;
                continue;
            }
            int32_t v = ((int32_t)c->s->pcm[idx] - 128) << 8;
            l += v * c->left / 127;
            r += v * c->right / 127;
            c->pos += c->s->step;
        }
        if (l > 32767)
            l = 32767;
        if (l < -32768)
            l = -32768;
        if (r > 32767)
            r = 32767;
        if (r < -32768)
            r = -32768;
        mixbuf[i * 2] = (int16_t)l;
        mixbuf[i * 2 + 1] = (int16_t)r;
    }
    mixed += frames;
}

static boolean snd_init(boolean prefix)
{
    use_prefix = prefix;
    dsp = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (dsp < 0) {
        printf("doom: sem som (/dev/dsp: %s)\n", strerror(errno));
        return false;
    }
    int fmt = AFMT_S16_LE, ch = 2, rate = MIX_RATE;
    if (ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt) < 0 || ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
        ioctl(dsp, SNDCTL_DSP_SPEED, &rate) < 0 || fmt != AFMT_S16_LE || ch != 2) {
        printf("doom: sem som (o /dev/dsp recusou o formato)\n");
        close(dsp);
        dsp = -1;
        return false;
    }
    memset(channels, 0, sizeof(channels));
    printf("doom: som: %d canais mixados a %d Hz, escrita sem bloqueio no /dev/dsp\n", NUM_CHANNELS, rate);
    return true;
}

static void snd_shutdown(void)
{
    if (dsp >= 0)
        close(dsp);
    dsp = -1;
}

static int snd_lump(sfxinfo_t *sfx)
{
    char name[16];
    if (sfx->link)
        sfx = sfx->link;
    if (use_prefix)
        snprintf(name, sizeof(name), "ds%s", sfx->name);
    else
        snprintf(name, sizeof(name), "%s", sfx->name);
    return W_CheckNumForName(name);
}

static void snd_update(void)
{
    if (dsp < 0)
        return;
    audio_buf_info info;
    if (ioctl(dsp, SNDCTL_DSP_GETOSPACE, &info) < 0)
        return;
    static int total;
    if (info.fragstotal * info.fragsize > total)
        total = info.fragstotal * info.fragsize;
    if (info.bytes > total)
        total = info.bytes;
    int queued = total - info.bytes;
    int target = MIX_RATE * 4 * TARGET_MS / 1000;
    if (queued >= target)
        return;
    uint32_t frames = (uint32_t)(target - queued) / 4;
    if (frames > (uint32_t)info.bytes / 4)
        frames = (uint32_t)info.bytes / 4;
    if (frames > MIX_FRAMES)
        frames = MIX_FRAMES;
    if (!frames)
        return;
    mix(frames);
    ssize_t w = write(dsp, mixbuf, frames * 4);
    if (w < 0 && errno == EAGAIN)
        eagain++;
    else if (w > 0)
        writes++;
}

static void snd_params(int channel, int vol, int sep)
{
    if (channel >= 0 && channel < NUM_CHANNELS)
        set_params(&channels[channel], vol, sep);
}

static int snd_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (dsp < 0 || channel < 0 || channel >= NUM_CHANNELS)
        return -1;
    const struct sample *s = load(sfx);
    if (!s)
        return -1;
    struct channel *c = &channels[channel];
    c->s = s;
    c->pos = 0;
    set_params(c, vol, sep);
    started++;
    return channel;
}

static void snd_stop(int channel)
{
    if (channel >= 0 && channel < NUM_CHANNELS)
        channels[channel].s = NULL;
}

static boolean snd_playing(int channel)
{
    return channel >= 0 && channel < NUM_CHANNELS && channels[channel].s != NULL;
}

static void snd_cache(sfxinfo_t *sounds, int num)
{
    (void)sounds;
    (void)num;
}

static snddevice_t devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    devices,
    sizeof(devices) / sizeof(devices[0]),
    snd_init,
    snd_shutdown,
    snd_lump,
    snd_update,
    snd_params,
    snd_start,
    snd_stop,
    snd_playing,
    snd_cache,
};

static boolean mus_init(void)
{
    return false;
}

static void mus_nothing(void)
{
}

static void mus_volume(int v)
{
    (void)v;
}

static void *mus_register(void *data, int len)
{
    (void)data;
    (void)len;
    return NULL;
}

static void mus_unregister(void *h)
{
    (void)h;
}

static void mus_play(void *h, boolean loop)
{
    (void)h;
    (void)loop;
}

static boolean mus_playing(void)
{
    return false;
}

music_module_t DG_music_module = {
    NULL,
    0,
    mus_init,
    mus_nothing,
    mus_volume,
    mus_nothing,
    mus_nothing,
    mus_register,
    mus_unregister,
    mus_play,
    mus_nothing,
    mus_playing,
    NULL,
};

void orion_sound_report(void)
{
    if (dsp < 0)
        return;
    printf("doom: som: %lu efeito(s) iniciado(s), %lu quadro(s) mixado(s), %lu escrita(s), %lu EAGAIN\n", started,
           mixed, writes, eagain);
}
