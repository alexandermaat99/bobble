#pragma once
// The 4" square board (Waveshare ESP32-S3-Touch-LCD-4B): an ST7701 RGB panel (set up over a 3-wire SPI that runs
// through the TCA9554 I/O expander), a PWM backlight on GPIO4, GT911 touch and an AXP2101 power chip.
// Only used on the real board: the Square simulator plays the round board's hardware.
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if defined(BOARD_SQUARE) && !defined(BOBBLE_SIM)
#define SQUARE_HW 1
#else
#define SQUARE_HW 0
#endif

i2c_master_bus_handle_t sq_i2c(void);   // the board's one I2C bus (made on first use: the panel, touch and battery share it)
void sq_display_init(void);             // panel reset and setup, frame buffer, backlight (full on)
// Put rows y0..y1 (columns x0..x1, rows packed) of big-endian RGB565 into the frame buffer; `done` is given once px
// is free again, which may be after this returns
void sq_draw(int x0, int y0, int x1, int y1, uint16_t *px, SemaphoreHandle_t done);
// The same DMA copy into any PSRAM buffer (dst and bytes on 64-byte cache lines): `done` once it has landed
void sq_copy(uint16_t *dst, uint16_t *px, int bytes, SemaphoreHandle_t done);
void sq_backlight(int level);           // 0 (off) .. 255
void sq_touch_init(void);
bool sq_touch_read(int *x, int *y);     // a finger is down (screen pixels); a tap between two reads shows once
void sq_touch_fast(bool on);            // read the chip every 10 ms (screen on), else every 100 ms
void sq_touch_sleep(bool on);           // stop reading the touch chip (the PWR button's off) / start again
float sq_battery_volts(void);           // the cell's voltage from the power chip; 0 if no battery
bool sq_plugged_in(void);               // USB power is present (a computer or a wall charger)
int sq_power_key(void);                 // the PWR button since last asked: 0 none, 1 short press, 2 long press
#include "battery.h"
void sq_power_read(battery_diag_t *d);  // everything the power chip says about charging (a dozen register reads)
void sq_display_recover(int what);     // 1: the LCD controller set up again, 2: the panel chip's setup sent again
void sq_expander_check(void);          // logs the expander's state when it changes (and if it isn't as left)
bool sq_amp(bool on);                   // the speaker's amplifier (through the expander): off unless something's playing
