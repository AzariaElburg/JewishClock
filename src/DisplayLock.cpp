#include "DisplayLock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_mutex = nullptr;
}

void DisplayLock::begin() {
    s_mutex = xSemaphoreCreateMutex();
}

void DisplayLock::take() {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void DisplayLock::give() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}
