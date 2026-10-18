#pragma once

#include "win.h"

#include <poll.h>

#define APP_SOCKET "/run/wmapp.sock"
#define APP_DIR    "/run/wm"
#define APP_MAX    6

void app_init(struct desktop *d, struct gfx *g);
void app_shutdown(void);
int  app_pollfds(struct pollfd *fds, int max);
void app_service(const struct pollfd *fds, int n);
void app_key(struct window *w, int32_t ascii, int32_t scancode, int32_t mods);
void app_click(struct window *w, int x, int y, uint32_t buttons);
void app_close_window(struct window *w);
