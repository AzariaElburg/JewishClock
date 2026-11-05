#pragma once
#include "esp_err.h"
#include "WiFi.h"
inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t* ps) { if (ps) *ps = WIFI_PS_NONE; return ESP_OK; }
esp_err_t esp_wifi_scan_stop();   // sim_wifi.cpp
