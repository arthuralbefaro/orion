#pragma once

#include <stdint.h>

#define PIT_BASE_FREQUENCY 1193182u

void pit_init(uint32_t hz);
void pit_channel2_wait_ms(uint32_t ms);