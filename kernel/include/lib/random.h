#pragma once

#include <kernel/compiler.h>

uint64_t random_u64(void);
void random_fill(void *buf, size_t len);
