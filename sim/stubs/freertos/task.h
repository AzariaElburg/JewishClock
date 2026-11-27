#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void*);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* param,
                                   UBaseType_t prio, TaskHandle_t* handle, BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t handle);
// The download task sleeps in simulated time (sim_platform.cpp); on the
// main loop it does nothing. A no-op for the task too made its wait for a
// still screen (Calendar.cpp, since 2026-10-30) spin while simulated time
// stood still - runs froze from the first calendar cache save on.
void vTaskDelay(TickType_t ticks);
// Stack use can't be measured on the PC: no handle, 0 (the clock leaves the key out).
inline TaskHandle_t xTaskGetHandle(const char*) { return nullptr; }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
