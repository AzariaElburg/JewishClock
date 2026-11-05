#pragma once
// PC stand-in (jcclock): the 20 s loop watchdog, kept by sim_clock_main.cpp -
// a loop() that doesn't come back in time restarts the simulated clock with
// reset reason TASK_WDT, as on the clock.
#include "esp_err.h"
#include <cstdint>
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* cfg);
esp_err_t esp_task_wdt_add(void* task);
esp_err_t esp_task_wdt_reset();
