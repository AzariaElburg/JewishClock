#pragma once
// PC stand-in (jcclock): mDNS starts fine and answers nothing - the PC
// reaches the simulator by its port (sim/README.md).
#include <Arduino.h>
class MDNSResponder {
public:
    bool begin(const char*) { return true; }
    void end() {}
    void setInstanceName(const char*) {}
    bool addService(const char*, const char*, uint16_t) { return true; }
    bool addServiceTxt(const char*, const char*, const char*, const char*) { return true; }
};
extern MDNSResponder MDNS;
