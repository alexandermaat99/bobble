#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// Pet page: a pair of round white robot eyes, the page left of the calendar
// tx = -1 when no finger. up: the board's up vector in glass axes (x right, y down, z out of the glass);
// ang: how far the content is turned on the glass (degrees clockwise), to work out "downhill" on the page.
// sleep_in: microseconds until the screen turns itself off (the eyes get drowsy just before)
void pet_ui_update(int tx, int ty, int64_t now_us, const float up[3], float ang, int64_t sleep_in);
void pet_ui_wake(void);                                   // screen just came on
void pet_ui_cancel_touch(void);                           // a swipe took over the finger
void pet_ui_draw(uint16_t *buf, int y0, int h);
// The start of a meeting when it's within 3 minutes of starting or under 2 minutes in (the screen stays on
// for it), else 0. Works with the screen off too.
time_t pet_ui_meeting_window(void);
bool pet_ui_dirty(void);
bool pet_ui_bubble_open(void);
// First run: 1 = no Wi-Fi yet (a bubble says hello, a tap opens Wi-Fi setup), 2 = online but no Mac yet (points to
// Agent Bridge), 0 = set up
void pet_ui_set_welcome(int stage);
bool pet_ui_wants_wifi(void);                             // the welcome bubble was tapped: open Wi-Fi setup (once)                            // an agent's message is up (the screen stays on for it)                                  // something moved since the last call
// Call when a dirty frame is about to be drawn: gives the area that changed (false: redraw everything);
// any pointer may be NULL just to note that the frame is drawn
bool pet_ui_take_rect(int *x0, int *y0, int *x1, int *y1);
// The same, as up to two separate areas ({x0, y0, x1, y1} each): the eyes and bubble, and the logos along the bottom
// (far apart: one box round both would be most of the page). 0 = redraw the whole page.
int pet_ui_take_rects(int r[2][4]);
