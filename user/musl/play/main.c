#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>

struct sound {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    uint8_t *data;
    size_t len;
};

static void usage(void)
{
    fprintf(stderr,
            "uso: play arquivo.wav [--nao-bloqueia]\n"
            "     play --tom HZ MS [--taxa N] [--gravar arquivo.wav] [--nao-bloqueia]\n"
            "     play --teste-ocupado\n");
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int load_wav(const char *path, struct sound *s)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "play: %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        fprintf(stderr, "play: %s: leitura falhou\n", path);
        return -1;
    }
    fclose(f);
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "play: %s nao e WAV\n", path);
        return -1;
    }
    long pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= size) {
        uint32_t len = le32(buf + pos + 4);
        const uint8_t *body = buf + pos + 8;
        if (!memcmp(buf + pos, "fmt ", 4) && len >= 16) {
            if (le16(body) != 1) {
                fprintf(stderr, "play: so PCM\n");
                return -1;
            }
            s->channels = le16(body + 2);
            s->rate = le32(body + 4);
            s->bits = le16(body + 14);
            have_fmt = true;
        } else if (!memcmp(buf + pos, "data", 4) && have_fmt) {
            s->data = (uint8_t *)body;
            s->len = (size_t)len <= (size_t)(size - (pos + 8)) ? len : (size_t)(size - (pos + 8));
            return 0;
        }
        pos += 8 + len + (len & 1);
    }
    fprintf(stderr, "play: %s sem fmt ou data\n", path);
    return -1;
}

static int make_tone(struct sound *s, double hz, uint32_t ms, uint32_t rate)
{
    size_t frames = (size_t)rate * ms / 1000;
    int16_t *pcm = malloc(frames * 4);
    if (!pcm)
        return -1;
    for (size_t i = 0; i < frames; i++) {
        pcm[i * 2] = (int16_t)(12000.0 * sin(2.0 * M_PI * hz * (double)i / rate));
        pcm[i * 2 + 1] = 0;
    }
    s->rate = rate;
    s->channels = 2;
    s->bits = 16;
    s->data = (uint8_t *)pcm;
    s->len = frames * 4;
    return 0;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static int save_wav(const char *path, const struct sound *s)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "play: %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint32_t block = s->channels * s->bits / 8;
    fwrite("RIFF", 1, 4, f);
    put32(f, (uint32_t)(36 + s->len));
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, (uint16_t)s->channels);
    put32(f, s->rate);
    put32(f, s->rate * block);
    put16(f, (uint16_t)block);
    put16(f, (uint16_t)s->bits);
    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)s->len);
    fwrite(s->data, 1, s->len, f);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    printf("play: gravado %s, %u Hz, %u canal(is), %u bits, %zu bytes\n", path, s->rate, s->channels, s->bits,
           s->len);
    return 0;
}

static int set(int fd, unsigned long req, int want, const char *what)
{
    int v = want;
    if (ioctl(fd, req, &v) < 0) {
        fprintf(stderr, "play: ioctl %s: %s\n", what, strerror(errno));
        return -1;
    }
    return v;
}

static int play(const struct sound *s, bool nonblock)
{
    int fd = open("/dev/dsp", O_WRONLY | (nonblock ? O_NONBLOCK : 0));
    if (fd < 0) {
        fprintf(stderr, "play: /dev/dsp: %s\n", strerror(errno));
        return 1;
    }
    int fmt = set(fd, SNDCTL_DSP_SETFMT, s->bits == 8 ? AFMT_U8 : AFMT_S16_LE, "SETFMT");
    int ch = set(fd, SNDCTL_DSP_CHANNELS, (int)s->channels, "CHANNELS");
    int rate = set(fd, SNDCTL_DSP_SPEED, (int)s->rate, "SPEED");
    if (fmt < 0 || ch < 0 || rate < 0) {
        close(fd);
        return 1;
    }
    double secs = (double)s->len / ((double)s->rate * s->channels * s->bits / 8);
    printf("play: %u Hz, %d canal(is), %u bits, %.2f s%s\n", s->rate, ch, s->bits, secs,
           nonblock ? ", sem bloqueio" : "");
    size_t off = 0;
    unsigned long again = 0, waits = 0;
    while (off < s->len) {
        size_t n = s->len - off < 4096 ? s->len - off : 4096;
        ssize_t w = write(fd, s->data + off, n);
        if (w < 0) {
            if (errno == EAGAIN) {
                again++;
                audio_buf_info info;
                do {
                    struct timespec ts = { 0, 5 * 1000 * 1000 };
                    nanosleep(&ts, NULL);
                    waits++;
                } while (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes < info.fragsize);
                continue;
            }
            fprintf(stderr, "play: write: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        off += (size_t)w;
    }
    close(fd);
    if (nonblock)
        printf("play: fim, %zu bytes; %lu espera(s) pelo GETOSPACE, %lu EAGAIN\n", s->len, waits, again);
    else
        printf("play: fim, %zu bytes\n", s->len);
    return 0;
}

static int busy_test(void)
{
    int a = open("/dev/dsp", O_WRONLY);
    if (a < 0) {
        fprintf(stderr, "play: /dev/dsp: %s\n", strerror(errno));
        return 1;
    }
    int b = open("/dev/dsp", O_WRONLY);
    int err = errno;
    if (b >= 0) {
        printf("play: segundo open do /dev/dsp NAO deu EBUSY\n");
        close(b);
        close(a);
        return 1;
    }
    close(a);
    int c = open("/dev/dsp", O_WRONLY);
    printf("play: segundo open: %s; depois do close o open volta a funcionar: %s\n",
           err == EBUSY ? "EBUSY" : strerror(err), c >= 0 ? "sim" : "nao");
    if (c >= 0)
        close(c);
    return err == EBUSY && c >= 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *save = NULL;
    double hz = 0;
    uint32_t ms = 0, rate = 44100;
    bool nonblock = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tom") && i + 2 < argc) {
            hz = atof(argv[++i]);
            ms = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--taxa") && i + 1 < argc) {
            rate = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gravar") && i + 1 < argc) {
            save = argv[++i];
        } else if (!strcmp(argv[i], "--nao-bloqueia")) {
            nonblock = true;
        } else if (!strcmp(argv[i], "--teste-ocupado")) {
            return busy_test();
        } else if (argv[i][0] != '-' && !file) {
            file = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    struct sound s;
    memset(&s, 0, sizeof(s));
    if (hz > 0 && ms > 0) {
        if (rate < 4000 || rate > 96000 || make_tone(&s, hz, ms, rate) < 0) {
            fprintf(stderr, "play: tom invalido\n");
            return 1;
        }
    } else if (file) {
        if (load_wav(file, &s) < 0)
            return 1;
        printf("play: %s\n", file);
    } else {
        usage();
        return 2;
    }
    if (save)
        return save_wav(save, &s) < 0 ? 1 : 0;
    if ((s.bits != 8 && s.bits != 16) || s.channels < 1 || s.channels > 2) {
        fprintf(stderr, "play: so 8 ou 16 bits, 1 ou 2 canais\n");
        return 1;
    }
    return play(&s, nonblock);
}
