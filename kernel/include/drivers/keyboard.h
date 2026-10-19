/*
 * keyboard.h - teclado PS/2 (controlador 8042, scancode set 1).
 *
 * A IRQ1 lê o scancode, traduz para ASCII e guarda num ring buffer que o
 * kernel consome fora da interrupção (keyboard_getchar).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void keyboard_init(void);
void keyboard_feed(uint8_t scancode);

/* Retorna o próximo caractere ou -1 se não há nada pendente. */
int keyboard_getchar(void);

/* Estado dos modificadores (bits). */
#define KBD_MOD_SHIFT (1 << 0)
#define KBD_MOD_CTRL  (1 << 1)
#define KBD_MOD_ALT   (1 << 2)
#define KBD_MOD_CAPS  (1 << 3)
#define KBD_MOD_EXTENDED (1 << 4)
uint8_t keyboard_modifiers(void);
