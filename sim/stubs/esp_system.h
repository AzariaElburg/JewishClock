#pragma once
#include <cstdint>
// PC stand-in (simulator only). Deterministic, so runs are repeatable.
uint32_t esp_random();
typedef enum { ESP_RST_UNKNOWN = 0, ESP_RST_POWERON = 1 } esp_reset_reason_t;
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
