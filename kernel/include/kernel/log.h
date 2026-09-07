#pragma once

#include <lib/printf.h>

enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

void klog(enum log_level level, const char *subsystem, const char *fmt, ...) PRINTF_LIKE(3, 4);

#define log_debug(sub, ...) klog(LOG_DEBUG, sub, __VA_ARGS__)
#define log_info(sub, ...)  klog(LOG_INFO,  sub, __VA_ARGS__)
#define log_warn(sub, ...)  klog(LOG_WARN,  sub, __VA_ARGS__)
#define log_error(sub, ...) klog(LOG_ERROR, sub, __VA_ARGS__)

NORETURN void panic(const char *fmt, ...) PRINTF_LIKE(1, 2);

#define ASSERT(cond)                                                       \
    do {                                                                   \
        if (unlikely(!(cond)))                                             \
            panic("assert falhou: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)