#pragma once
#include <stdarg.h>
#include <stdint.h>
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
typedef int (*vprintf_like_t)(const char *, va_list);
void esp_log_write(esp_log_level_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void esp_log_level_set(const char *tag, esp_log_level_t level);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t f);
uint32_t esp_log_timestamp(void);
#define ESP_LOGE(tag, fmt, ...) esp_log_write(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_log_write(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_log_write(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { if (0) esp_log_write(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { if (0) esp_log_write(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__); } while (0)
