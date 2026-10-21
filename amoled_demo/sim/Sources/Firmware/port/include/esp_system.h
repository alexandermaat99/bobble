#pragma once
#include "esp_err.h"
#include "esp_heap_caps.h"
void esp_restart(void) __attribute__((noreturn));
uint32_t esp_get_free_heap_size(void);
