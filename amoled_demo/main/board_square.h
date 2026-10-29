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
void sq_backlight(int level);           // 0 (off) .. 255
void sq_touch_init(void);
bool sq_touch_read(int *x, int *y);     // a finger is down (screen pixels)
float sq_battery_volts(void);           // the cell's voltage from the power chip; 0 if no battery
bool sq_plugged_in(void);               // USB power is present (a computer or a wall charger)
