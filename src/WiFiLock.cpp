#include "WiFiLock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_mutex = nullptr;
}

void WiFiLock::begin() {
    s_mutex = xSemaphoreCreateMutex();
}

void WiFiLock::take() {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void WiFiLock::give() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}

bool WiFiLock::heldByCurrentTask() {
    // xSemaphoreGetMutexHolder() only works on a real mutex (created via
    // xSemaphoreCreateMutex(), as s_mutex is) - not a generic counting
    // semaphore. Treat "not created yet" (startup ordering) as unverifiable
    // rather than a violation.
    if (!s_mutex) return true;
    return xSemaphoreGetMutexHolder(s_mutex) == xTaskGetCurrentTaskHandle();
}
