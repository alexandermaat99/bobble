#pragma once
#include <stdbool.h>

// Screen brightness the user picked in Settings (10..255); the sleep fade comes back up to it
int screen_brightness_get(void);
void screen_brightness_set(int level, bool save);
// The board's own volume, picked in Settings (0..100, 0: silent); saved in NVS
int device_volume_get(void);
void device_volume_set(int level, bool save);
// The look (the square): Classic, Field (Teenage Engineering-ish: black, four loud colours, monospace labels, a
// segmented clock) or Tiles (like a phone's widgets: rounded tiles in flat bright colours on black, nested in each
// other, big numbers). Picked in Settings, saved in NVS
enum { LOOK_CLASSIC, LOOK_FIELD, LOOK_TILES, N_LOOKS };
int ui_look(void);
void ui_look_set(int look);
bool ui_field(void);   // (the look is Field)
bool ui_tiles(void);   // (or Tiles)
// Units for temperatures (and later anything else measured): saved in NVS, Fahrenheit until someone picks otherwise
enum { UNITS_F, UNITS_C, N_UNITS };
int ui_units(void);
void ui_units_set(int units);
// The motion sensor, for GET /motion: its filtered reading (g, the sensor's own axes), "up" in screen axes (x right,
// y down, z out of the glass), the angle the content is turned to, and whether a tilt calibration is saved
void motion_get(float raw[3], float up[3], float *angle, bool *calibrated, float R[9]);
void motion_turn(int degrees);
void motion_tilt_cal(bool on);   // (testing, GET /motion?tilt=1 / 0) start or stop the tilt calibration
int motion_tilt_step(float *progress);   // its step (0: not calibrating) and how long this one's been held (s)   // (testing, GET /motion?turn=90) turn the screen there until the board's tipped again

// A power test (POST /power, testing): two settings in turn with the screen held on, the battery's voltage averaged
// under each. -1 in a field: as usual. bright 0..255; wifi 0 awake, 1 min modem sleep, 2 max; cpu MHz (80/160/240);
// chg: the charge current (mA, on USB: switching it gives the battery's resistance, to turn volts into mA)
typedef struct { int bright, wifi, cpu, chg; } power_cfg_t;
void power_test_start(const power_cfg_t *a, const power_cfg_t *b, int half_s, int total_s);
void power_test_stop(void);
int power_test_json(char *out, int max);
