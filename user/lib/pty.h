#pragma once

#define TIOCINQ   0x541B
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

int openpty(int *master, int *slave);
