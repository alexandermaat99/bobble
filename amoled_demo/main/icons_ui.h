#pragma once
#include <stdint.h>

// Integration logos for the agent's bubble (generated into icons_ui.c by tools/make_icons.py): a 44x44 4-bit
// coverage mask each (two pixels a byte, high nibble first) and the brand's colour, made light enough for black.
// The table ends with a NULL key.
#define ICON_SIZE 44
typedef struct { const char *key; const uint8_t *mask; uint8_t r, g, b; } icon_t;
extern const icon_t icons_ui[];
