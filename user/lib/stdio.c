#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static FILE files[3] = { { .fd = 0 }, { .fd = 1 }, { .fd = 2 } };
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    FILE *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->fd = fd;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    if (mode[0] != 'r') {
        errno = EACCES;
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

int fclose(FILE *f)
{
    if (!f)
        return EOF;
    int r = close(f->fd);
    if (f != stdin && f != stdout && f != stderr)
        free(f);
    return r;
}

int fflush(FILE *f) { (void)f; return 0; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fileno(FILE *f) { return f->fd; }

static int fill(FILE *f)
{
    ssize_t n = read(f->fd, f->buf, BUFSIZ);
    if (n <= 0) {
        if (n < 0) f->err = 1; else f->eof = 1;
        return 0;
    }
    f->rpos = 0;
    f->rlen = (int)n;
    return 1;
}

int fgetc(FILE *f)
{
    if (f->rpos >= f->rlen && !fill(f))
        return EOF;
    return f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

char *fgets(char *buf, int size, FILE *f)
{
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buf[i] = '\0';
    return buf;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n, done = 0;
    unsigned char *out = buf;
    while (done < total) {
        if (f->rpos >= f->rlen && !fill(f))
            break;
        size_t avail = (size_t)(f->rlen - f->rpos);
        size_t take = total - done < avail ? total - done : avail;
        memcpy(out + done, f->buf + f->rpos, take);
        f->rpos += (int)take;
        done += take;
    }
    return size ? done / size : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    size_t total = size * n;
    ssize_t w = write(f->fd, buf, total);
    if (w < 0) { f->err = 1; return 0; }
    return size ? (size_t)w / size : 0;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return write(f->fd, &ch, 1) == 1 ? c : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    return write(f->fd, s, n) == (ssize_t)n ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF)
        return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 1;
}

int fseek(FILE *f, long off, int whence)
{
    f->rpos = f->rlen = 0;
    f->eof = 0;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
    long pos = lseek(f->fd, 0, SEEK_CUR);
    return pos < 0 ? -1 : pos - (f->rlen - f->rpos);
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

struct out {
    char *buf;
    size_t size;
    size_t pos;
};

static void put(struct out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        put(o, c);
}

static void fmt_num(struct out *o, unsigned long v, int base, int upper, int neg, int width, int zero, int left, int alt)
{
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    int prefix = neg + (alt && base == 16 ? 2 : 0);
    int total = n + prefix;
    if (!left && !zero) pad(o, ' ', width - total);
    if (neg) put(o, '-');
    if (alt && base == 16) { put(o, '0'); put(o, upper ? 'X' : 'x'); }
    if (!left && zero) pad(o, '0', width - total);
    while (n) put(o, tmp[--n]);
    if (left) pad(o, ' ', width - total);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { put(&o, *p); continue; }
        p++;
        int left = 0, zero = 0, alt = 0, width = 0, prec = -1, lng = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '#') alt = 1;
            else break;
        }
        if (*p == '*') { width = va_arg(ap, int); p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') { p++; prec = 0; if (*p == '*') { prec = va_arg(ap, int); p++; } else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        while (*p == 'l' || *p == 'z' || *p == 'h') { if (*p == 'l' || *p == 'z') lng = 1; p++; }
        switch (*p) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            fmt_num(&o, v < 0 ? (unsigned long)(-v) : (unsigned long)v, 10, 0, v < 0, width, zero, left, 0);
            break;
        }
        case 'u': fmt_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, 0, width, zero, left, 0); break;
        case 'x': fmt_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, 0, width, zero, left, alt); break;
        case 'X': fmt_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 1, 0, width, zero, left, alt); break;
        case 'o': fmt_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 8, 0, 0, width, zero, left, 0); break;
        case 'p': fmt_num(&o, (unsigned long)va_arg(ap, void *), 16, 0, 0, width, zero, left, 1); break;
        case 'c': { if (!left) pad(&o, ' ', width - 1); put(&o, (char)va_arg(ap, int)); if (left) pad(&o, ' ', width - 1); break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            if (!left) pad(&o, ' ', width - len);
            for (int i = 0; i < len; i++) put(&o, s[i]);
            if (left) pad(&o, ' ', width - len);
            break;
        }
        case '%': put(&o, '%'); break;
        case '\0': p--; break;
        default: put(&o, '%'); put(&o, *p); break;
        }
    }
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = '\0';
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, 1 << 30, fmt, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[1024];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (n >= (int)sizeof(buf)) {
        char *big = malloc((size_t)n + 1);
        if (big) {
            vsnprintf(big, (size_t)n + 1, fmt, ap);
            write(fd, big, (size_t)n);
            free(big);
            return n;
        }
        n = (int)sizeof(buf) - 1;
    }
    write(fd, buf, (size_t)n);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) { return vdprintf(f->fd, fmt, ap); }
int vprintf(const char *fmt, va_list ap) { return vdprintf(1, fmt, ap); }

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(f->fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}
