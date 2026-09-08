#pragma once

#include <kernel/compiler.h>

void tty_init(void);
void tty_input(char c);
void tty_write(const void *buf, uint64_t len);
int64_t tty_read(void *buf, uint64_t len);
