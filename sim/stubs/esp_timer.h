#pragma once
// PC stand-in (simulator only - see sim/README.md): the ESP-IDF microsecond
// timer, on the simulator's own clock.
#include <Arduino.h>
int64_t simUptimeUs();   // sim_platform.cpp: 64-bit, never wraps (micros() does, as on the ESP32)
inline int64_t esp_timer_get_time() { return simUptimeUs(); }
