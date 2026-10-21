#pragma once
#include <stdint.h>
#include <stdbool.h>

void cal_ui_init(void);
void cal_ui_enter(void);
// Feed the current touch (x = -1 when up). Returns true when the user tapped to move to the next screen.
bool cal_ui_update(int tx, int ty, int64_t now_us);
void cal_ui_render_bg(uint16_t *buf, int y0, int y1);   // safe to call from both render cores
void cal_ui_draw(uint16_t *buf, int y0, int h);
bool cal_ui_animating(void);
bool cal_ui_is_face(void);
void cal_ui_cancel_touch(void);    // a page swipe took the finger     // on the clock face (not the scrolling agenda)   // something is moving (agenda glide): keep redrawing
