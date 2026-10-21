#pragma once
// The emulator's side of the hardware: what the Mac app sets (mouse, tilt, buttons, battery) and what the
// firmware's drawing leaves (the glass), shared between the port's files
#include <stdbool.h>
#include <stdint.h>
const char *sim_nvs_path(void);       // the "flash"
const char *sim_support_dir(void);    // ~/Library/Application Support/BobbleSim
bool sim_nvs_exists(void);
int sim_http_port(void);              // where the web server listens (the board's port 80)
void sim_set_http_port(int port);
