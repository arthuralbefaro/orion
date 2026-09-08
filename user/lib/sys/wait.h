#pragma once

#include <sys/types.h>

pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);

#define WEXITSTATUS(s) ((s) & 0xFF)
#define WIFEXITED(s)   (1)
