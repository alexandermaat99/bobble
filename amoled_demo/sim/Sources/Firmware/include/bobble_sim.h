#pragma once
// Bobble's firmware running on the Mac. The app starts it, shows the glass and feeds in the mouse, tilt and buttons.
#include <stdbool.h>
#include <stdint.h>

void sim_start(int argc, char **argv);     // runs app_main on its own thread (as on the board)
// The glass: sim_glass_side() pixels square (466 round, or 480 for the square board: BOARD_SQUARE), RGB565,
// big-endian as the panel takes it. Copies it out if it changed since `seq` (returns the new
// seq); brightness 0..255 and whether the panel is on (asleep: off).
int sim_glass_side(void);
bool sim_glass_round(void);
uint32_t sim_copy_glass(uint16_t *out, uint32_t seq);
int sim_brightness(void);
bool sim_panel_on(void);
void sim_touch(int x, int y, bool down);   // glass pixels
void sim_boot_button(bool down);
void sim_set_tilt(float degrees, bool flat);   // turned clockwise on the desk, or lying flat on its back
void sim_shake(void);                      // picked up (wakes it)
void sim_set_battery(int mv, bool usb);    // the cell's voltage, and on a computer's USB (charging)
int sim_battery_mv(void);
bool sim_usb(void);
int sim_http_port(void);
const char *sim_support_dir(void);
