#pragma once
#include <stdbool.h>
#include <time.h>
#include "ical.h"
#include "esp_http_server.h"

// Downloads the calendars in the background (every 5 minutes while online) and keeps the coming week's occurrences
// ready for the screen. Which calendars (their private .ics links) is set from the Mac's Bobble app
// (POST /calendars {"urls": [...]}) and kept in NVS; none to begin with.
void calendar_start(void);
void calendar_register(httpd_handle_t server);   // GET/POST /calendars (with the display key)
void calendar_refresh(void);                 // fetch again soon
void calendar_set_awake(bool on);            // screen on/off: refreshes every 5 min awake, 30 min asleep
int calendar_items(ical_item_t *out, int max);
time_t calendar_window_start(void);          // local midnight of "today" for the current items
const char *calendar_error(void);            // "" when the last fetch worked
bool calendar_loaded(void);
int calendar_count(void);                    // how many calendars are configured
