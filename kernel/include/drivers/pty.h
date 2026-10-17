#pragma once

#include <kernel/compiler.h>

#define TIOCINQ  0x541B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TCGETS    0x5401
#define TCSETS    0x5402
#define TCSETSW   0x5403
#define TCSETSF   0x5404
#define IS_TCSETS(r) ((r) == TCSETS || (r) == TCSETSW || (r) == TCSETSF)
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
#define TIO_ISIG   0x0001
#define TIO_ECHOE  0x0010
#define TIO_ECHOK  0x0020
#define TIO_IEXTEN 0x8000
#define TIO_ICRNL  0x0100
#define TIO_OPOST  0x0001
#define TIO_ONLCR  0x0004
#define TIO_B38400 0x000F
#define TIO_CS8    0x0030
#define TIO_CREAD  0x0080
#define TIO_VINTR  0
#define TIO_VQUIT  1
#define TIO_VERASE 2
#define TIO_VKILL  3
#define TIO_VEOF   4
#define TIO_VTIME  5
#define TIO_VMIN   6

struct user_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct file;

int pty_open_pair(struct file **master, struct file **slave);
uint32_t pty_count(void);
void pty_dump(void);
