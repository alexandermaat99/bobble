#pragma once
#include <stddef.h>
#include "esp_err.h"
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 0)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C (1 << 2)
static inline esp_err_t esp_cache_msync(void *addr, size_t size, int flags) { return ESP_OK; }   // (no cache to drop)
