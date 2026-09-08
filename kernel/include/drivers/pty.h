#pragma once

#include <kernel/compiler.h>

#define TIOCINQ  0x541B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

struct file;

int pty_open_pair(struct file **master, struct file **slave);
uint32_t pty_count(void);
void pty_dump(void);
