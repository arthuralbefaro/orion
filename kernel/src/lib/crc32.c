#include <lib/crc32.h>

static uint32_t table[256];
static bool table_ready;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    __atomic_store_n(&table_ready, true, __ATOMIC_RELEASE);
}

uint32_t crc32(const void *data, size_t len)
{
    if (!__atomic_load_n(&table_ready, __ATOMIC_ACQUIRE))
        build_table();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
