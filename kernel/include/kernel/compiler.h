/*
 * compiler.h - macros utilitárias que dependem de extensões do compilador
 * (clang/gcc). Tudo que é "mágica de atributo" fica centralizado aqui.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PACKED          __attribute__((packed))
#define ALIGNED(n)      __attribute__((aligned(n)))
#define NORETURN        __attribute__((noreturn))
#define UNUSED          __attribute__((unused))
#define USED            __attribute__((used))
#define SECTION(s)      __attribute__((section(s)))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))

#define ALIGN_UP(x, a)      (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((a) - 1)) == 0)
#define DIV_ROUND_UP(x, a)  (((x) + (a) - 1) / (a))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define KiB(x) ((uint64_t)(x) << 10)
#define MiB(x) ((uint64_t)(x) << 20)
#define GiB(x) ((uint64_t)(x) << 30)
