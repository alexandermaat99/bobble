#pragma once
#include <stdint.h>

// The loading screen while a board starts up (Wi-Fi, the clock, the calendar): one shape turning and changing, after
// Android's loading indicator - a scalloped cookie, a pentagon, a pill, a burst, a clover - in Google's four colours.
// t: seconds it's been showing. a: how much of it shows (1: all, over black; less: fading into the page under it).
// what: the line under it ("Joining Wi-Fi"), or NULL
void boot_ui_draw(uint16_t *buf, int y0, int h, float t, float a, const char *what);
