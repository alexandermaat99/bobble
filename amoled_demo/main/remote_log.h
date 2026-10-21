#pragma once
#include "esp_http_server.h"

// Keeps the last ~16 KB of log output in memory so it can be read over Wi-Fi (GET /log, token-protected)
void remote_log_start(void);
void remote_log_register(httpd_handle_t server);
