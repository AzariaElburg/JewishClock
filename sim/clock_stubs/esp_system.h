#pragma once
#include <cstdint>
// PC stand-in (jcclock): the reset reason is what the supervisor started this
// run for (sim_clock_main.cpp) - power on, restart, crash, watchdog.
uint32_t esp_random();
typedef enum { ESP_RST_UNKNOWN = 0, ESP_RST_POWERON = 1, ESP_RST_EXT = 2, ESP_RST_SW = 3, ESP_RST_PANIC = 4,
               ESP_RST_INT_WDT = 5, ESP_RST_TASK_WDT = 6, ESP_RST_WDT = 7 } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();
