#pragma once

#include <sys/types.h>

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#define WNOHANG 1

#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
