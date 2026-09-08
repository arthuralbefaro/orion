#pragma once

#define TIOCINQ 0x541B

int openpty(int *master, int *slave);
