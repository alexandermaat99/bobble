#pragma once
#include "esp_http_server.h"

void ota_register(httpd_handle_t server);   // adds POST /ota to the web server
int ota_progress(void);                     // -1 when idle, else 0..100 while an update is uploading
void ota_confirm_if_healthy(void);          // call regularly; confirms a new image after 30 s of uptime
const char *ota_build_time(void);           // "Oct 13 2026 16:40" of the running firmware
