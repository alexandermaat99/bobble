#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return e_; } } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) { ESP_LOGE(tag, __VA_ARGS__); return err; } } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, ...) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { ESP_LOGE(log_tag, __VA_ARGS__); ret = e_; goto goto_tag; } } while (0)
#define ESP_GOTO_ON_FALSE(a, err, goto_tag, log_tag, ...) do { if (!(a)) { ESP_LOGE(log_tag, __VA_ARGS__); ret = err; goto goto_tag; } } while (0)
