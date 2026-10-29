#pragma once
#include <stdint.h>
#include <stdbool.h>

void spotify_ui_update(int tx, int ty, int64_t now_us);   // touch in (x = -1 when up)
void spotify_ui_cancel_touch(void);                       // a swipe took over the finger
void spotify_ui_render_bg(uint16_t *buf, int y0, int y1); // safe to call from both render cores
void spotify_ui_draw(uint16_t *buf, int y0, int h);
bool spotify_ui_dirty(void);                              // something changed since the last call
bool spotify_ui_reels_dirty(void);                        // the square's Field reels turned...
int spotify_ui_take_reels(int r[2][4]);                   // ...where ({x0, y0, x1, y1} each; 0: they didn't)
