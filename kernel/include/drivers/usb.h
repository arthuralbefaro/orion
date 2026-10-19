#pragma once

#include <kernel/compiler.h>

bool usb_init(void);
size_t usb_report(char *buf, size_t cap);
