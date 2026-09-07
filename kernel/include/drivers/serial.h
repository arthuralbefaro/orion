#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SERIAL_COM1 0x3F8

void serial_init(void);
bool serial_available(void);
void serial_putc(char c);
void serial_write(const char *s);

int serial_getc(void);