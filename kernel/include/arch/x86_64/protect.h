#pragma once

#include <stdint.h>

#define RFLAGS_AC (1ull << 18)

extern uint8_t smep_enabled;
extern uint8_t smap_enabled;

void cpu_protect_init(void);
