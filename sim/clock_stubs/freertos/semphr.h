#pragma once
// PC stand-in (jcclock): mutexes that really lock and know their owner
// (WiFiLock::heldByCurrentTask() asks) - sim/sim_clock_main.cpp.
#include "FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t m);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t m);
