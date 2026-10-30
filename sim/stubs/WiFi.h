#pragma once
// PC stand-in (simulator only): online from Sim::wifiUpMs after "power on",
// as measured on the clock.
#include <Arduino.h>
#include "WiFiClientSecure.h"
#include "sim_platform.h"

typedef int WiFiEvent_t;
typedef struct { int dummy; } WiFiEventInfo_t;

typedef enum { WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3,
               WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 } wl_status_t;

class IPAddress {
public:
    IPAddress(uint8_t a = 0, uint8_t b = 0, uint8_t c = 0, uint8_t d = 0) : a_(a), b_(b), c_(c), d_(d) {}
    String toString() const { char s[20]; snprintf(s, sizeof(s), "%u.%u.%u.%u", a_, b_, c_, d_); return s; }
private:
    uint8_t a_, b_, c_, d_;
};

class WiFiClass {
public:
    wl_status_t status() { return millis() >= Sim::wifiUpMs ? WL_CONNECTED : WL_DISCONNECTED; }
    void begin(const char*, const char*) {}
    IPAddress localIP() { return IPAddress(127, 0, 0, 1); }
    int RSSI() { return -60; }
    String SSID() { return "simulator"; }
};
extern WiFiClass WiFi;
