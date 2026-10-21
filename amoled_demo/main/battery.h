#pragma once
#include <stdbool.h>

// Battery on the board's 3.7 V lithium connector, read through GPIO4 (ADC1 channel 3, divided by 3)
void battery_init(void);
void battery_update(void);       // call regularly, asleep too; it samples about twice a second
void battery_set_screen(bool on);  // screen load changed: corrects for the voltage sag it causes
float battery_volts(void);       // smoothed and screen-sag corrected, 0 if not read yet
int battery_percent(void);       // 0..100, or -1 when no battery seems connected
bool battery_charging(void);     // plugged into a computer's USB (a wall charger can't be sensed)
int battery_learned_runs(void);  // full discharges learned from (0: generic LiPo table)
bool battery_logging(void);      // currently recording a discharge from full
bool battery_empty(void);         // at 0% on battery for a minute: time to shut down until it's charged
float battery_empty_volts(void);  // where 0% is
float battery_read_now(void);     // one immediate reading (no smoothing), for the check at power-up
