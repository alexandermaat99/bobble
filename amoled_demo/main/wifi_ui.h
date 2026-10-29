#pragma once
#include <stdint.h>
#include <stdbool.h>

// Settings card: menu (Wi-Fi, calibrate touch, calibrate tilt, lock) and the Wi-Fi screens
// (network list, on-screen keyboard, connection status).
enum { WIFI_UI_NONE, WIFI_UI_CLOSE, WIFI_UI_CAL_TOUCH, WIFI_UI_CAL_TILT, WIFI_UI_LOCK, WIFI_UI_RESET };   // LOCK/RESET: held for 3 s
void wifi_ui_enter(void);
void wifi_ui_show_phone(void);           // (the card just opened) go straight to "set up from your phone"
void wifi_ui_leave(void);
// Feed the current touch (x = -1 when not touching). Returns a WIFI_UI_* action for the caller.
int wifi_ui_update(int tx, int ty, int64_t now_us);
void wifi_ui_draw(uint16_t *buf, int y0, int h);
void wifi_ui_cancel_touch(void);
bool wifi_ui_swipe_ok(void);
bool wifi_ui_slider_rect(int r[4]);         // the brightness slider is being dragged: its box (card = glass when open)
bool wifi_ui_wants_status_bar(void);     // an upward drag anywhere may close the card   // forget the current press (a screen gesture took over)
