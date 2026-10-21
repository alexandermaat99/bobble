#pragma once
#include "FreeRTOS.h"
typedef struct sim_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core);
#define xTaskCreate(fn, name, stack, arg, prio, out) xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, tskNO_AFFINITY)
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
static inline void vTaskPrioritySet(TaskHandle_t t, UBaseType_t p) {}
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) { return 4096; }
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
