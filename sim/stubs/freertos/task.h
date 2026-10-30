#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void*);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* param,
                                   UBaseType_t prio, TaskHandle_t* handle, BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t handle);
inline void vTaskDelay(TickType_t) {}
