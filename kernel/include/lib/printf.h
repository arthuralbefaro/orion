#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <kernel/compiler.h>

typedef void (*printf_putc_fn)(char c, void *ctx);

int kvformat(printf_putc_fn put, void *ctx, const char *fmt, va_list ap);

int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_LIKE(3, 4);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

int kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
int kvprintf(const char *fmt, va_list ap);

void kputc(char c);
void kputs(const char *s);
