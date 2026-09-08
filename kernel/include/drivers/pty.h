#pragma once

#include <kernel/compiler.h>

#define TIOCINQ 0x541B

struct file;

int pty_open_pair(struct file **master, struct file **slave);
uint32_t pty_count(void);
void pty_dump(void);
