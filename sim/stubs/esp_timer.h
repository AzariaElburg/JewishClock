#pragma once
// PC stand-in (simulator only - see sim/README.md): the ESP-IDF microsecond
// timer, on the simulator's own clock.
#include <Arduino.h>
inline int64_t esp_timer_get_time() { return (int64_t)micros(); }
