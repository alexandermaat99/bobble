#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>   // (ESP-IDF headers bring it in)
#include "sdkconfig.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xb004
#define ESP_ERR_HTTP_BASE 0x7000
const char *esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { fprintf(stderr, "ESP_ERROR_CHECK failed: %s at %s:%d\n", esp_err_to_name(e_), __FILE__, __LINE__); abort(); } } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)
