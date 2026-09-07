#include <lib/printf.h>
#include <lib/string.h>
#include <drivers/serial.h>
#include <drivers/console.h>
#include <kernel/spinlock.h>

#define FLAG_LEFT   (1 << 0)  
#define FLAG_ZERO   (1 << 1)  
#define FLAG_ALT    (1 << 2)  
#define FLAG_SPACE  (1 << 3)  
#define FLAG_PLUS   (1 << 4)  
#define FLAG_UPPER  (1 << 5)  

struct out {
    printf_putc_fn put;
    void *ctx;
    int count;
};

static inline void emit(struct out *o, char c)
{
    o->put(c, o->ctx);
    o->count++;
}

static void emit_padding(struct out *o, char c, int n)
{
    while (n-- > 0)
        emit(o, c);
}

static void emit_field(struct out *o, const char *s, int len, int width, unsigned flags)
{
    int pad = width - len;
    if (!(flags & FLAG_LEFT))
        emit_padding(o, ' ', pad);
    for (int i = 0; i < len; i++)
        emit(o, s[i]);
    if (flags & FLAG_LEFT)
        emit_padding(o, ' ', pad);
}

static void emit_number(struct out *o, uint64_t value, bool negative, unsigned base,
                        unsigned flags, int width, int precision)
{
    char digits[64 + 1];
    const char *alphabet = (flags & FLAG_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    int ndigits = 0;

    if (value == 0) {
        digits[ndigits++] = '0';
    } else {
        while (value) {
            digits[ndigits++] = alphabet[value % base];
            value /= base;
        }
    }

    if (precision == 0 && ndigits == 1 && digits[0] == '0')
        ndigits = 0;

    int zeros = (precision > ndigits) ? precision - ndigits : 0;

    char prefix[3];
    int prefix_len = 0;
    if (negative)
        prefix[prefix_len++] = '-';
    else if (flags & FLAG_PLUS)
        prefix[prefix_len++] = '+';
    else if (flags & FLAG_SPACE)
        prefix[prefix_len++] = ' ';

    if ((flags & FLAG_ALT) && base == 16 && ndigits > 0) {
        prefix[prefix_len++] = '0';
        prefix[prefix_len++] = (flags & FLAG_UPPER) ? 'X' : 'x';
    } else if ((flags & FLAG_ALT) && base == 8 && zeros == 0 && !(ndigits > 0 && digits[ndigits - 1] == '0')) {
        prefix[prefix_len++] = '0';
    }

    int total = prefix_len + zeros + ndigits;

    if ((flags & FLAG_ZERO) && !(flags & FLAG_LEFT) && precision < 0 && width > total) {
        zeros += width - total;
        total = width;
    }

    int pad = width - total;
    if (!(flags & FLAG_LEFT))
        emit_padding(o, ' ', pad);

    for (int i = 0; i < prefix_len; i++)
        emit(o, prefix[i]);
    emit_padding(o, '0', zeros);
    while (ndigits > 0)
        emit(o, digits[--ndigits]);

    if (flags & FLAG_LEFT)
        emit_padding(o, ' ', pad);
}

enum length_mod { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T };

int kvformat(printf_putc_fn put, void *ctx, const char *fmt, va_list ap)
{
    struct out o = { .put = put, .ctx = ctx, .count = 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(&o, *p);
            continue;
        }

        p++;

        unsigned flags = 0;
        for (;; p++) {
            if (*p == '-')      flags |= FLAG_LEFT;
            else if (*p == '0') flags |= FLAG_ZERO;
            else if (*p == '#') flags |= FLAG_ALT;
            else if (*p == ' ') flags |= FLAG_SPACE;
            else if (*p == '+') flags |= FLAG_PLUS;
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            if (*p == '*') {
                precision = va_arg(ap, int);
                if (precision < 0)
                    precision = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    precision = precision * 10 + (*p++ - '0');
            }
        }

        enum length_mod len = LEN_NONE;
        if (*p == 'h') {
            p++;
            if (*p == 'h') { len = LEN_HH; p++; } else len = LEN_H;
        } else if (*p == 'l') {
            p++;
            if (*p == 'l') { len = LEN_LL; p++; } else len = LEN_L;
        } else if (*p == 'z') {
            len = LEN_Z; p++;
        } else if (*p == 't') {
            len = LEN_T; p++;
        }

        switch (*p) {
        case 'd':
        case 'i': {
            int64_t v;
            switch (len) {
            case LEN_HH: v = (signed char)va_arg(ap, int); break;
            case LEN_H:  v = (short)va_arg(ap, int); break;
            case LEN_L:  v = va_arg(ap, long); break;
            case LEN_LL: v = va_arg(ap, long long); break;
            case LEN_Z:  v = (int64_t)va_arg(ap, size_t); break;
            case LEN_T:  v = va_arg(ap, ptrdiff_t); break;
            default:     v = va_arg(ap, int); break;
            }
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            emit_number(&o, mag, neg, 10, flags, width, precision);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            switch (len) {
            case LEN_HH: v = (unsigned char)va_arg(ap, unsigned); break;
            case LEN_H:  v = (unsigned short)va_arg(ap, unsigned); break;
            case LEN_L:  v = va_arg(ap, unsigned long); break;
            case LEN_LL: v = va_arg(ap, unsigned long long); break;
            case LEN_Z:  v = va_arg(ap, size_t); break;
            case LEN_T:  v = (uint64_t)va_arg(ap, ptrdiff_t); break;
            default:     v = va_arg(ap, unsigned); break;
            }
            unsigned base = (*p == 'u') ? 10 : (*p == 'o') ? 8 : 16;
            if (*p == 'X')
                flags |= FLAG_UPPER;
            emit_number(&o, v, false, base, flags, width, precision);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            emit_number(&o, v, false, 16, flags | FLAG_ALT, width, 16);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            emit_field(&o, &c, 1, width, flags);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int slen = (int)((precision >= 0) ? strnlen(s, (size_t)precision) : strlen(s));
            emit_field(&o, s, slen, width, flags);
            break;
        }
        case '%':
            emit(&o, '%');
            break;
        case '\0':
            emit(&o, '%');
            return o.count;
        default:
            emit(&o, '%');
            emit(&o, *p);
            break;
        }
    }

    return o.count;
}

struct buf_ctx {
    char *buf;
    size_t size;
    size_t pos;
};

static void buf_putc(char c, void *ctx)
{
    struct buf_ctx *b = ctx;
    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct buf_ctx b = { .buf = buf, .size = size, .pos = 0 };
    int n = kvformat(buf_putc, &b, fmt, ap);
    if (size > 0)
        buf[(b.pos < size) ? b.pos : size - 1] = '\0';
    return n;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kputc(char c)
{
    serial_putc(c);
    console_putc(c);
}

void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

static void kputc_cb(char c, void *ctx)
{
    (void)ctx;
    kputc(c);
}

static spinlock_t print_lock = SPINLOCK_INIT;

int kvprintf(const char *fmt, va_list ap)
{
    uint64_t flags = irq_save();
    bool locked = spin_try_lock(&print_lock);
    int n = kvformat(kputc_cb, NULL, fmt, ap);
    if (locked)
        spin_unlock(&print_lock);
    irq_restore(flags);
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}