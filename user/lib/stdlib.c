#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

struct block {
    size_t size;
    struct block *next;
    int free;
};

#define ALIGN16(x) (((x) + 15) & ~(size_t)15)
#define MIN_SPLIT 32

static struct block *heap_head;

void *malloc(size_t size)
{
    if (size == 0)
        size = 1;
    size = ALIGN16(size);
    for (struct block *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
                struct block *rest = (struct block *)((char *)(b + 1) + size);
                rest->size = b->size - size - sizeof(struct block);
                rest->free = 1;
                rest->next = b->next;
                b->next = rest;
                b->size = size;
            }
            b->free = 0;
            return b + 1;
        }
    }
    size_t chunk = ALIGN16(sizeof(struct block) + size);
    if (chunk < 65536)
        chunk = 65536;
    struct block *b = sbrk((long)chunk);
    if (b == (void *)-1)
        return NULL;
    b->size = chunk - sizeof(struct block);
    b->free = 1;
    b->next = NULL;
    if (!heap_head) {
        heap_head = b;
    } else {
        struct block *t = heap_head;
        while (t->next) t = t->next;
        t->next = b;
    }
    return malloc(size);
}

void free(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = (struct block *)ptr - 1;
    b->free = 1;
    for (struct block *c = heap_head; c && c->next; c = c->next) {
        if (c->free && c->next->free && (char *)(c + 1) + c->size == (char *)c->next) {
            c->size += sizeof(struct block) + c->next->size;
            c->next = c->next->next;
        }
    }
}

void *calloc(size_t n, size_t size)
{
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    struct block *b = (struct block *)ptr - 1;
    if (b->size >= size)
        return ptr;
    void *np = malloc(size);
    if (!np)
        return NULL;
    memcpy(np, ptr, b->size);
    free(ptr);
    return np;
}

long strtol(const char *s, char **end, int base)
{
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    long v = 0;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (isalpha(*s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtol(s, end, base); }
int  atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int  abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

char *getenv(const char *name)
{
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    return NULL;
}

void exit(int code)
{
    fflush(stdout);
    _exit(code);
}

void abort(void)
{
    fputs("abort()\n", stderr);
    _exit(134);
}

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--)
            swap_bytes(b + (j - 1) * size, b + j * size, size);
}

static unsigned rand_state = 12345;
int rand(void) { rand_state = rand_state * 1103515245 + 12345; return (int)((rand_state >> 16) & 0x7FFF); }
void srand(unsigned seed) { rand_state = seed; }

#define ENV_MAX   64
#define ENV_ENTRY 256

static char env_store[ENV_MAX][ENV_ENTRY];
static char *env_slots[ENV_MAX + 1];
static int env_owned;

static void env_adopt(void)
{
    if (env_owned)
        return;
    int n = 0;
    for (char **e = environ; e && *e && n < ENV_MAX; e++) {
        size_t k = 0;
        while ((*e)[k] && k < ENV_ENTRY - 1) {
            env_store[n][k] = (*e)[k];
            k++;
        }
        env_store[n][k] = '\0';
        env_slots[n] = env_store[n];
        n++;
    }
    env_slots[n] = NULL;
    environ = env_slots;
    env_owned = 1;
}

static int env_find(const char *name)
{
    size_t len = strlen(name);
    for (int i = 0; env_slots[i]; i++)
        if (strncmp(env_slots[i], name, len) == 0 && env_slots[i][len] == '=')
            return i;
    return -1;
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !*name || strchr(name, '='))
        return -1;
    env_adopt();
    int i = env_find(name);
    if (i >= 0 && !overwrite)
        return 0;
    if (i < 0) {
        for (i = 0; i < ENV_MAX && env_slots[i]; i++)
            ;
        if (i >= ENV_MAX)
            return -1;
        env_slots[i] = env_store[i];
        env_slots[i + 1] = NULL;
    }
    snprintf(env_store[i], ENV_ENTRY, "%s=%s", name, value);
    env_slots[i] = env_store[i];
    return 0;
}

int unsetenv(const char *name)
{
    env_adopt();
    int i = env_find(name);
    if (i < 0)
        return 0;
    int last = i;
    while (env_slots[last + 1])
        last++;
    env_slots[i] = env_slots[last];
    env_slots[last] = NULL;
    return 0;
}

int putenv(char *entry)
{
    char *eq = strchr(entry, '=');
    if (!eq)
        return unsetenv(entry);
    char name[128];
    size_t n = (size_t)(eq - entry);
    if (n >= sizeof(name))
        return -1;
    memcpy(name, entry, n);
    name[n] = '\0';
    return setenv(name, eq + 1, 1);
}
