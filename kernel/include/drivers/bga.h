#pragma once

#include <kernel/compiler.h>

bool bga_init(void);
bool bga_present(void);
int bga_set_mode(uint32_t width, uint32_t height);
