#pragma once
#include <stdint.h>

// Battery status drawn at the very top of every page: bolt when charging, cell icon, percentage
void draw_status_bar(uint16_t *buf, int y0, int h, int top);   // top: row of the bar's top edge
