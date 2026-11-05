#pragma once
// PC stand-in (jcclock): real UDP from the PC (sim/sim_net.cpp) - the clock's
// NTPManager.cpp asks real time servers. Nothing leaves while the simulated
// clock's network has no internet (Sim::wifiInternet), as on the clock.
#include <Arduino.h>
#include "WiFi.h"
class WiFiUDP {
public:
    uint8_t begin(uint16_t port);
    void stop();
    int beginPacket(IPAddress ip, uint16_t port);
    size_t write(const uint8_t* buf, size_t n);
    size_t write(uint8_t c) { return write(&c, 1); }
    int endPacket();
    int parsePacket();
    int read(uint8_t* buf, size_t n);
    int read(char* buf, size_t n) { return read((uint8_t*)buf, n); }
    int available();
private:
    intptr_t sock_ = -1;
    uint32_t toIp_ = 0; uint16_t toPort_ = 0;
    std::string out_, in_; size_t inPos_ = 0;
};
