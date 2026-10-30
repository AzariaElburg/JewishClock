#include "NTPLock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_mutex = nullptr;
}

void NTPLock::begin() {
    s_mutex = xSemaphoreCreateMutex();
}

void NTPLock::take() {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void NTPLock::give() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}
