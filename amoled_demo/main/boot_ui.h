#pragma once
#include <stdint.h>

// The loading screen while a board starts up (Wi-Fi, the clock, the calendar): one shape turning and changing, after
// Android's loading indicator - a scalloped cookie, a pentagon, a pill, a burst, a clover - in Google's four colours.
// boot_ui_frame once a frame, before drawing: t, the time (s); a, how much of it shows (1: all, over black; less:
// fading into the page under it). what: the line under it ("Joining Wi-Fi"), or NULL
void boot_ui_frame(float t, float a);
void boot_ui_draw(uint16_t *buf, int y0, int h, const char *what);
