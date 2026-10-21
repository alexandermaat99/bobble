#pragma once
#include "FreeRTOS.h"
typedef struct sim_sem *SemaphoreHandle_t;
SemaphoreHandle_t sim_sem_new(unsigned max, unsigned initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
#define xSemaphoreCreateMutex() sim_sem_new(1, 1)
#define xSemaphoreCreateBinary() sim_sem_new(1, 0)
#define xSemaphoreCreateCounting(max, init) sim_sem_new(max, init)
#define xSemaphoreGiveFromISR(s, woken) xSemaphoreGive(s)
#define vSemaphoreDelete(s) ((void)(s))
