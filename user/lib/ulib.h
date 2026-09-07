#pragma once

#include <stdint.h>
#include <stddef.h>

int main(void);

long   write(int fd, const void *buf, size_t len);
void   exit(int code) __attribute__((noreturn));
int    getpid(void);
void   yield(void);
void   sleep_ms(unsigned long ms);

size_t strlen(const char *s);
void   puts(const char *s);
void   print_num(unsigned long n);
