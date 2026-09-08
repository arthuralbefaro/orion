#pragma once

#include <sys/types.h>

#define INPUT_EV_KEY_DOWN     1
#define INPUT_EV_KEY_UP       2
#define INPUT_EV_MOUSE_MOVE   3
#define INPUT_EV_MOUSE_BUTTON 4

#define INPUT_MOD_SHIFT (1 << 0)
#define INPUT_MOD_CTRL  (1 << 1)
#define INPUT_MOD_ALT   (1 << 2)
#define INPUT_MOD_CAPS  (1 << 3)

#define INPUT_BUTTON_LEFT   (1 << 0)
#define INPUT_BUTTON_RIGHT  (1 << 1)
#define INPUT_BUTTON_MIDDLE (1 << 2)

#define INPUTIOC_SETTIMEOUT 0x4900
#define INPUTIOC_FLUSH      0x4901

struct input_event {
    uint32_t type;
    int32_t  a;
    int32_t  b;
    int32_t  c;
};
