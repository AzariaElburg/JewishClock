#pragma once
// PC stand-in (jcclock). The simulated flash outside LittleFS (two firmware
// slots and their states) lives in <state>/flash/ - sim/sim_ota.cpp.
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
typedef struct { int slot; const char* label; uint32_t size; } esp_partition_t;
// FlashStats.cpp wraps these (pioarduino.ini -Wl,--wrap=...); in the
// simulator nothing calls them: LittleFS is a folder on the PC.
extern "C" {
esp_err_t __real_esp_partition_read(const esp_partition_t* p, size_t off, void* dst, size_t n);
esp_err_t __real_esp_partition_write(const esp_partition_t* p, size_t off, const void* src, size_t n);
esp_err_t __real_esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n);
}
