#pragma once
// PC stand-in (jcclock): the captive-portal DNS. Not needed on the PC - the
// test browser maps 192.168.4.1 itself (sim/README.md).
#include "WiFi.h"
class DNSServer {
public:
    bool start(uint16_t, const String&, const IPAddress&) { return true; }
    void stop() {}
    void processNextRequest() {}
};
