#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"

// Sim racing: a bridge on the rig PC (tools/race_bridge.py, reading iRacing's telemetry) POSTs /race about 10 times a
// second while you're on track, and the screen becomes a lap delta display: the gap to your best lap, big, in green
// (faster) or red (slower), with the lap, position, last and best laps. It goes back to the usual pages a few seconds
// after the posts stop.
//   POST /race  (X-OTA-Token)  {"delta": -0.214, "valid": true, "lap": 7, "laps": 20, "pos": 3, "last": 92.481,
//                               "best": 91.902, "track": "Spa"}          (all optional; times in seconds, 0: none yet)
void race_register(httpd_handle_t server);
bool race_active(void);    // telemetry heard in the last few seconds: the race screen is up
bool race_dirty(void);     // something on it changed since the last frame (clears it)
void race_draw(uint16_t *buf, int y0, int h);
