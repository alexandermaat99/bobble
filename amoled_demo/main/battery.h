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

// What the charger says, for Settings > Battery and GET /battery: the square's power chip (AXP2101) reports all of it;
// elsewhere only usb and vbat_mv are known (chip false). -1: not known.
typedef struct {
    bool chip;                   // the power chip answered
    bool usb, battery;           // USB power present; a battery connected
    bool charge_on;              // charging switched on in the chip
    int dir;                     // the battery's current: 0 none, 1 charging, 2 discharging
    int stage;                   // 0 trickle, 1 pre-charge, 2 constant current, 3 constant voltage, 4 done, 5 stopped
    int vbat_mv, vbus_mv, vsys_mv, gauge_pct;
    int charge_ma, full_mv, in_limit_ma;   // the chip's settings: charge current, voltage it charges to, USB input limit
    float chip_c;                // the chip's own temperature
    uint8_t st1, st2, ts, irq[3];   // raw: status 1 and 2, TS pin control, interrupt status (what tripped)
} battery_diag_t;
void battery_diag(battery_diag_t *d);   // the latest (refreshed every second by battery_update)
const char *battery_state_text(const battery_diag_t *d);   // "Charging", "Charged", "On battery", "No battery"...
const char *battery_stage_text(int stage);
bool battery_charging_now(const battery_diag_t *d);   // current is going into the battery (else: plugged in and charging)
