/*
 * string.c - implementação freestanding de memcpy/memset & cia.
 *
 * memcpy/memset usam "rep movsb"/"rep stosb": em CPUs modernas (ERMSB) isso é
 * tão rápido quanto código SSE, e nós compilamos com SSE desligado no kernel.
 */
#include <lib/string.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    void *ret = dst;
    __asm__ volatile("rep movsb"
                     : "+D"(dst), "+S"(src), "+c"(n)
                     :
                     : "memory");
    return ret;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    __asm__ volatile("rep stosb"
                     : "+D"(dst), "+c"(n)
                     : "a"((uint8_t)c)
                     : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;

    if (d < s || d >= s + n) {
        /* Sem sobreposição perigosa: cópia para frente. */
        __asm__ volatile("rep movsb"
                         : "+D"(d), "+S"(s), "+c"(n)
                         :
                         : "memory");
        return dst;
    }

    /* dst está dentro de src: copia de trás para frente. */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strcpy(char *restrict dst, const char *restrict src)
{
    char *ret = dst;
    while ((*dst++ = *src++) != '\0')
        ;
    return ret;
}

/* Copia no máximo size-1 chars e sempre termina com NUL. Retorna strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

bool str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}
