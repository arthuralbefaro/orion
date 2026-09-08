/*
 * serial.h - porta serial COM1 (16550 UART).
 *
 * É a saída de debug mais importante do kernel: no QEMU, "-serial stdio"
 * joga tudo que o kernel escreve aqui direto no seu terminal.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SERIAL_COM1 0x3F8

void serial_init(void);
bool serial_available(void);
void serial_putc(char c);
void serial_write(const char *s);

/* Leitura não-bloqueante: retorna -1 se não há byte disponível. */
int serial_getc(void);
