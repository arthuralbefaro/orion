#pragma once

#include <kernel/compiler.h>
#include <kernel/wait.h>

void tty_init(void);
void tty_input(char c);
void tty_write(const void *buf, uint64_t len);
int64_t tty_read(void *buf, uint64_t len);
short tty_poll(struct poll_table *pt);
void tty_set_pgrp(int32_t pgid);
int32_t tty_get_pgrp(void);
