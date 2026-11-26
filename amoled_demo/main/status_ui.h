#pragma once
#include <stdint.h>
#include <stdbool.h>

// The Status page (left of the eyes, right of the voice chat): Bobble's eyes act out what you're up to (available,
// busy, focused, in a meeting, back soon, at lunch, away), with a row of little icons along the bottom to pick one.
// The pick is kept in flash; a calendar meeting switches to "in a meeting" by itself (and back after).
// tx = -1 when no finger. Call update every loop, whatever page is up (the calendar link needs it); the rest is for when
// the page is the one showing.
void status_ui_update(int tx, int ty, int64_t now_us);
void status_ui_cancel_touch(void);   // a page swipe took over the finger: it wasn't a tap
void status_ui_draw(uint16_t *buf, int y0, int h);
bool status_ui_dirty(void);          // the eyes are always doing something: true about 12 times a second
