#pragma once
#include "FreeRTOS.h"
typedef struct sim_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
#define xQueueSendToBack xQueueSend
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
