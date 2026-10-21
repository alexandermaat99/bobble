#pragma once
#include <stdint.h>
#include <stdbool.h>

void spotify_ui_update(int tx, int ty, int64_t now_us);   // touch in (x = -1 when up)
void spotify_ui_cancel_touch(void);                       // a swipe took over the finger
void spotify_ui_render_bg(uint16_t *buf, int y0, int y1); // safe to call from both render cores
void spotify_ui_draw(uint16_t *buf, int y0, int h);
bool spotify_ui_dirty(void);                              // something changed since the last call
