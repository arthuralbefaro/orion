#pragma once

#include <kernel/compiler.h>

#define TIOCINQ  0x541B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TCGETS    0x5401
#define TCSETS    0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct user_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#define TIO_ICANON 0x0002
#define TIO_ECHO   0x0008

struct user_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[32];
};

struct file;

int pty_open_pair(struct file **master, struct file **slave);
uint32_t pty_count(void);
void pty_dump(void);
