#pragma once
#include "esp_err.h"
typedef enum { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static inline esp_err_t gpio_config(const gpio_config_t *c) { return ESP_OK; }
int gpio_get_level(int pin);   // BOOT (0): low while held
