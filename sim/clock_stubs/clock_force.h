#pragma once
// Force-included into every file of the full simulator (sim/build_clock.py),
// after sim/stubs/sim_force.h: what the ESP32's Arduino.h brings along on the
// clock - FreeRTOS's types, tasks and locks.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
