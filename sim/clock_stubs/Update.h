#pragma once
// PC stand-in (jcclock): writes new firmware into the other slot of the
// simulated flash (sim/sim_ota.cpp); end(true) makes it the one to boot next,
// as "new" - the next start runs it on trial (ESP_OTA_IMG_PENDING_VERIFY).
#include <Arduino.h>
class UpdateClass {
public:
    bool begin(size_t size);
    size_t write(uint8_t* data, size_t len);
    bool end(bool evenIfRemaining = false);
    void abort();
    const char* errorString();
};
extern UpdateClass Update;
