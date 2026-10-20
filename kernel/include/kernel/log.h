#pragma once

#include <lib/printf.h>
#include <stdarg.h>

enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

#define LOG_LIMIT_PER_SECOND 10

struct log_limit {
    uint64_t window;
    uint32_t count;
    uint32_t dropped;
};

void klog(enum log_level level, const char *subsystem, const char *fmt, ...) PRINTF_LIKE(3, 4);
void kvlog(enum log_level level, const char *subsystem, const char *fmt, va_list ap);
void klog_limited(struct log_limit *lim, enum log_level level, const char *subsystem, const char *fmt, ...)
    PRINTF_LIKE(4, 5);

#define log_debug(sub, ...) klog(LOG_DEBUG, sub, __VA_ARGS__)
#define log_info(sub, ...)  klog(LOG_INFO,  sub, __VA_ARGS__)
#define log_warn(sub, ...)  klog(LOG_WARN,  sub, __VA_ARGS__)
#define log_error(sub, ...) klog(LOG_ERROR, sub, __VA_ARGS__)

#define log_warn_limited(sub, ...)                          \
    do {                                                    \
        static struct log_limit lim_;                       \
        klog_limited(&lim_, LOG_WARN, sub, __VA_ARGS__);    \
    } while (0)

#define log_error_limited(sub, ...)                         \
    do {                                                    \
        static struct log_limit lim_;                       \
        klog_limited(&lim_, LOG_ERROR, sub, __VA_ARGS__);   \
    } while (0)

NORETURN void panic(const char *fmt, ...) PRINTF_LIKE(1, 2);

#define ASSERT(cond)                                                       \
    do {                                                                   \
        if (unlikely(!(cond)))                                             \
            panic("assert falhou: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)
