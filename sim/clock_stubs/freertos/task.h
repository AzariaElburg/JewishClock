#pragma once
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void*);
// Calendar's download task: sim_platform.cpp, as in the screen simulator.
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* param,
                                   UBaseType_t prio, TaskHandle_t* handle, BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
// Stack use can't be measured on the PC: no handle, 0 (the clock leaves the key out).
inline TaskHandle_t xTaskGetHandle(const char*) { return nullptr; }
inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
TaskHandle_t xTaskGetCurrentTaskHandle();   // one handle per thread (sim_clock_main.cpp)
const char* pcTaskGetName(TaskHandle_t task);   // "loopTask" for the main loop, "async_tcp" for the web server
