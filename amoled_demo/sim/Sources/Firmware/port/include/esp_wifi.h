#pragma once
// main.c's few direct radio calls (the rest of Wi-Fi is sim_wifi.c, in place of wifi_setup.c)
#include "esp_err.h"
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
static inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { return ESP_OK; }
static inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t *t) { *t = WIFI_PS_NONE; return ESP_OK; }
esp_err_t esp_wifi_restore(void);
