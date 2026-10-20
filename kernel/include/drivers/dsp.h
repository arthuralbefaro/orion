#pragma once

#include <kernel/compiler.h>

struct file;

struct file *dsp_open(void);
int dsp_open_error(void);
