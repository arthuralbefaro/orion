#pragma once

#include <sys/types.h>

#define TCGETS    0x5401
#define TCSETS    0x5402
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define ICANON 0x0002
#define ECHO   0x0008

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[32];
};

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
void cfmakeraw(struct termios *t);
