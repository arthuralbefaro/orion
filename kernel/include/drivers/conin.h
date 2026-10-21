#pragma once

#include <kernel/compiler.h>

void conin_kick(void);
void conin_wait(uint64_t ms);
uint64_t conin_wakeups(void);
uint64_t conin_timeouts(void);
