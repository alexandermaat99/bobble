#pragma once
#include <stdbool.h>

// Screen brightness the user picked in Settings (10..255); the sleep fade comes back up to it
int screen_brightness_get(void);
void screen_brightness_set(int level, bool save);
// The board's own volume, picked in Settings (0..100, 0: silent); saved in NVS
int device_volume_get(void);
void device_volume_set(int level, bool save);
// The motion sensor, for GET /motion: its filtered reading (g, the sensor's own axes), "up" in screen axes (x right,
// y down, z out of the glass), the angle the content is turned to, and whether a tilt calibration is saved
void motion_get(float raw[3], float up[3], float *angle, bool *calibrated, float R[9]);
void motion_turn(int degrees);   // (testing, GET /motion?turn=90) turn the screen there until the board's tipped again

