#pragma once
// FreeRTOS on POSIX threads: tasks are threads, a tick is a millisecond
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_err.h"
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7fffffff
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
void sim_critical(bool enter);
#define taskENTER_CRITICAL(m) sim_critical(true)
#define taskEXIT_CRITICAL(m) sim_critical(false)
#define portENTER_CRITICAL(m) sim_critical(true)
#define portEXIT_CRITICAL(m) sim_critical(false)
#define portYIELD_FROM_ISR(x) ((void)(x))
