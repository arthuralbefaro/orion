#pragma once

#include <kernel/compiler.h>

#define FPU_STATE_SIZE 512

void fpu_init_cpu(void);
void fpu_save(void *area);
void fpu_restore(const void *area);
void fpu_init_state(void *area);
void fpu_reset_current(void);
void fpu_sanitize(void *area);
