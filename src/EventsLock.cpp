#include "EventsLock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_mutex = nullptr;
}

void EventsLock::begin() {
    s_mutex = xSemaphoreCreateMutex();
}

void EventsLock::take() {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void EventsLock::give() {
    if (s_mutex) xSemaphoreGive(s_mutex);
}
