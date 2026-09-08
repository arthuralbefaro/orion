#pragma once

#include <kernel/compiler.h>

#define MOUSE_BUTTON_LEFT   (1 << 0)
#define MOUSE_BUTTON_RIGHT  (1 << 1)
#define MOUSE_BUTTON_MIDDLE (1 << 2)

struct mouse_packet {
    int32_t dx;
    int32_t dy;
    int32_t dz;
    uint8_t buttons;
};

void mouse_init(void);
bool mouse_available(void);
bool mouse_has_wheel(void);
bool mouse_poll(struct mouse_packet *out);
uint64_t mouse_packet_count(void);
uint64_t mouse_resync_count(void);
