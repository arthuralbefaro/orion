#pragma once

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int   abs(int v);
long  labs(long v);
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *entry);
void  exit(int code) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
void  qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int   rand(void);
void  srand(unsigned seed);
