#pragma once
#include <stdbool.h>

// Screen brightness the user picked in Settings (10..255); the sleep fade comes back up to it
int screen_brightness_get(void);
void screen_brightness_set(int level, bool save);

