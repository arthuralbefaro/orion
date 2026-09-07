#pragma once

#include <stdbool.h>
#include <stdint.h>

void keyboard_init(void);

int keyboard_getchar(void);

#define KBD_MOD_SHIFT (1 << 0)
#define KBD_MOD_CTRL  (1 << 1)
#define KBD_MOD_ALT   (1 << 2)
#define KBD_MOD_CAPS  (1 << 3)
uint8_t keyboard_modifiers(void);