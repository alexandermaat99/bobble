#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_http_server.h"

// Apple Reminders, pushed from the Mac by reminders_sync (it can't be fetched from iCloud directly).
// Ticking one off on the board is reported back to the Mac, which completes it in Reminders.
typedef struct {
    char id[96];
    char title[80];
    char list[32];
    time_t due;
    bool has_time;      // due at a specific time, not just a day
    unsigned char r, g, b;   // list colour
} reminder_t;

void reminders_register(httpd_handle_t server);   // POST /reminders (from the Mac), GET /reminders/done
int reminders_get(reminder_t *out, int max);       // open reminders, due order, ticked ones left out
void reminders_complete(const char *id);           // ticked off on the board
