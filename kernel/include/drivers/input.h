#pragma once

#include <kernel/compiler.h>
#include <kernel/wait.h>

#define INPUT_EV_KEY_DOWN     1
#define INPUT_EV_KEY_UP       2
#define INPUT_EV_MOUSE_MOVE   3
#define INPUT_EV_MOUSE_BUTTON 4

#define INPUT_MOD_SHIFT (1 << 0)
#define INPUT_MOD_CTRL  (1 << 1)
#define INPUT_MOD_ALT   (1 << 2)
#define INPUT_MOD_CAPS  (1 << 3)

#define INPUTIOC_SETTIMEOUT 0x4900
#define INPUTIOC_FLUSH      0x4901

struct input_event {
    uint32_t type;
    int32_t  a;
    int32_t  b;
    int32_t  c;
};

struct file;

void input_init(void);
struct file *input_open(void);

bool input_post_key(uint8_t scancode, char ascii, uint8_t modifiers, bool pressed);
void input_post_mouse_move(int32_t dx, int32_t dy, int32_t dz);
void input_post_mouse_button(uint32_t buttons);

bool input_grabbed(void);
void input_grab(void);
void input_release(void);

int64_t input_read(void *buf, uint64_t len);
short input_poll(struct poll_table *pt);
void input_set_timeout(uint32_t ms);
uint32_t input_timeout(void);
void input_flush(void);

uint32_t input_queued(void);
uint64_t input_event_count(void);
uint64_t input_dropped_count(void);
