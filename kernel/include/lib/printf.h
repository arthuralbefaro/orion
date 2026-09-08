/*
 * printf.h - formatação de texto estilo printf, sem libc.
 *
 * Suporta: %c %s %d %i %u %x %X %o %p %% ; flags '-', '0', '#', ' ', '+';
 * largura e precisão (inclusive '*'); modificadores h, hh, l, ll, z, t.
 * Sem ponto flutuante (o kernel roda com SSE/x87 desligados).
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <kernel/compiler.h>

/* Callback de saída: recebe um caractere e um contexto opaco. */
typedef void (*printf_putc_fn)(char c, void *ctx);

/* Motor de formatação genérico. Retorna o número de caracteres emitidos. */
int kvformat(printf_putc_fn put, void *ctx, const char *fmt, va_list ap);

/* Formata em buffer (sempre termina com NUL se size > 0). Retorna quantos
 * caracteres teriam sido escritos, como snprintf da libc. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_LIKE(3, 4);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Imprime na saída padrão do kernel (serial + console gráfico, se houver). */
int kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
int kvprintf(const char *fmt, va_list ap);

/* Escreve um caractere/string diretamente na saída do kernel. */
void kputc(char c);
bool kprint_lock(uint64_t *flags);
void kprint_unlock(bool locked, uint64_t flags);
void kputs(const char *s);
