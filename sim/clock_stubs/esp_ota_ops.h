#pragma once
// PC stand-in (jcclock): the two firmware slots and their states, as the
// ESP-IDF bootloader keeps them with rollback on (sim/sim_ota.cpp).
#include "esp_partition.h"
typedef enum {
    ESP_OTA_IMG_NEW = 0, ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3, ESP_OTA_IMG_ABORTED = 4, ESP_OTA_IMG_UNDEFINED = -1
} esp_ota_img_states_t;
const esp_partition_t* esp_ota_get_running_partition();
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback();
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot();   // returns only if there is nothing to go back to
