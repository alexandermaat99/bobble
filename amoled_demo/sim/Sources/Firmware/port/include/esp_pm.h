#pragma once
#include "esp_err.h"
typedef struct { int max_freq_mhz, min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
static inline esp_err_t esp_pm_configure(const void *c) { return ESP_OK; }
