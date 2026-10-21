#pragma once
#include <stdint.h>
#include <stdbool.h>

void library_ui_enter(void);
void library_ui_cancel_touch(void);   // a page swipe took the finger
void library_ui_update(int tx, int ty, int64_t now_us);   // touch in (x = -1 when up)
void library_ui_draw(uint16_t *buf, int y0, int h);
bool library_ui_dirty(void);
bool library_ui_animating(void);   // list is being dragged or gliding                              // needs a redraw
