#pragma once
#include <stdint.h>
#include <stdbool.h>

// Battery status drawn at the very top of every page: bolt when charging, cell icon, percentage. On the square board
// it sits in the top right corner instead, phone style: the time top left, the Wi-Fi fan and a white cell, no number (top is then ignored).
void draw_status_bar(uint16_t *buf, int y0, int h, int top);   // top: row of the bar's top edge
void status_bar_clock(bool show);   // (the square) the time top left: not on the calendar, which has its own big clock
