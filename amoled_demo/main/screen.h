#pragma once
#include <stdbool.h>

// Screen brightness the user picked in Settings (10..255); the sleep fade comes back up to it
int screen_brightness_get(void);
void screen_brightness_set(int level, bool save);
// The board's own volume, picked in Settings (0..100, 0: silent); saved in NVS
int device_volume_get(void);
void device_volume_set(int level, bool save);

