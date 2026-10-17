#include <lib/random.h>
#include <arch/x86_64/cpu.h>

static uint64_t random_state;

uint64_t random_u64(void)
{
    uint64_t x = __atomic_load_n(&random_state, __ATOMIC_RELAXED);
    uint64_t next;
    do {
        next = x ? x : (rdtsc() | 1);
        next ^= next << 13;
        next ^= next >> 7;
        next ^= next << 17;
    } while (!__atomic_compare_exchange_n(&random_state, &x, next, false, __ATOMIC_RELAXED,
                                          __ATOMIC_RELAXED));
    return next ^ rdtsc();
}

void random_fill(void *buf, size_t len)
{
    uint8_t *out = buf;
    size_t done = 0;
    while (done < len) {
        uint64_t r = random_u64();
        for (int i = 0; i < 8 && done < len; i++, done++)
            out[done] = (uint8_t)(r >> (i * 8));
    }
}
