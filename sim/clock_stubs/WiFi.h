#pragma once
// PC stand-in for the ESP32 Wi-Fi driver (the full simulator, jcclock).
// The networks around the clock come from a "world" (sim/sim_wifi.cpp): names,
// passwords, signal, access points, internet or not - changeable while it
// runs (sim/README.md, the control port). Joining takes the time measured on
// the clock and fails like the driver does (reason 201 not found, 202 wrong
// password); events reach the clock's handler from the main loop's side, as
// from the driver's own task.
#include <Arduino.h>
#include <functional>
#include <vector>
#include "WiFiClientSecure.h"
#include "sim_platform.h"

typedef enum { WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2, WL_CONNECTED = 3,
               WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6 } wl_status_t;
typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK } wifi_auth_mode_t;
typedef enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY } wifi_sort_method_t;
typedef enum { WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED  (-2)

typedef enum {
    ARDUINO_EVENT_WIFI_STA_CONNECTED = 4, ARDUINO_EVENT_WIFI_STA_DISCONNECTED = 5, ARDUINO_EVENT_WIFI_STA_GOT_IP = 7,
} arduino_event_id_t;
typedef arduino_event_id_t WiFiEvent_t;
typedef struct {
    struct { uint8_t ssid[33]; uint8_t ssid_len; uint8_t bssid[6]; uint8_t channel; uint8_t authmode; } wifi_sta_connected;
    struct { uint8_t ssid[33]; uint8_t ssid_len; uint8_t bssid[6]; uint8_t reason; int8_t rssi; } wifi_sta_disconnected;
    struct { struct { struct { uint32_t addr; } ip; } ip_info; } got_ip;
} WiFiEventInfo_t;
typedef std::function<void(WiFiEvent_t, WiFiEventInfo_t)> WiFiEventFuncCb;
typedef void (*WiFiEventSysCb)(WiFiEvent_t, WiFiEventInfo_t);

class IPAddress {
public:
    IPAddress() : a_(0), b_(0), c_(0), d_(0) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : a_(a), b_(b), c_(c), d_(d) {}
    explicit IPAddress(uint32_t addr) : a_(addr & 0xFF), b_((addr >> 8) & 0xFF), c_((addr >> 16) & 0xFF), d_(addr >> 24) {}   // lwIP order
    String toString() const { char s[20]; snprintf(s, sizeof(s), "%u.%u.%u.%u", a_, b_, c_, d_); return s; }
    bool operator==(const IPAddress& o) const { return a_ == o.a_ && b_ == o.b_ && c_ == o.c_ && d_ == o.d_; }
    bool operator!=(const IPAddress& o) const { return !(*this == o); }
    uint8_t operator[](int i) const { return i == 0 ? a_ : i == 1 ? b_ : i == 2 ? c_ : d_; }
    uint32_t addr() const { return (uint32_t)a_ | ((uint32_t)b_ << 8) | ((uint32_t)c_ << 16) | ((uint32_t)d_ << 24); }
    static IPAddress parse(const char* s) { unsigned a = 0, b = 0, c = 0, d = 0; sscanf(s ? s : "", "%u.%u.%u.%u", &a, &b, &c, &d); return IPAddress(a, b, c, d); }
private:
    uint8_t a_, b_, c_, d_;
};

class WiFiClass {
public:
    // Station
    wl_status_t begin(const char* ssid, const char* pass = nullptr);
    bool disconnect(bool wifiOff = false, bool eraseAp = false);
    wl_status_t status();
    IPAddress localIP();
    String SSID();
    int8_t RSSI();
    String BSSIDstr();
    int32_t channel();
    bool setHostname(const char*) { return true; }
    void setAutoReconnect(bool) {}
    void persistent(bool) {}
    bool setSleep(bool) { return true; }
    void setMinSecurity(wifi_auth_mode_t) {}
    void setScanMethod(wifi_scan_method_t) {}
    void setSortMethod(wifi_sort_method_t) {}
    int getTxPower() { return 78; }
    // Mode and the clock's own access point
    bool mode(wifi_mode_t m);
    wifi_mode_t getMode();
    bool softAP(const char* ssid, const char* pass = nullptr, int channel = 1, int hidden = 0, int maxConn = 4);
    bool softAPConfig(IPAddress, IPAddress, IPAddress) { return true; }
    bool softAPdisconnect(bool wifiOff = false);
    IPAddress softAPIP();
    // Scans (async only, as the clock uses them)
    int16_t scanNetworks(bool async = false, bool showHidden = false, bool passive = false, uint32_t msPerChannel = 300,
                         uint8_t channel = 0, const char* ssid = nullptr, const uint8_t* bssid = nullptr);
    int16_t scanComplete();
    void scanDelete();
    String SSID(uint8_t i);
    int32_t RSSI(uint8_t i);
    String BSSIDstr(uint8_t i);
    int32_t channel(uint8_t i);
    wifi_auth_mode_t encryptionType(uint8_t i);
    // Events
    int onEvent(WiFiEventSysCb cb) { handlers_.push_back(cb); return (int)handlers_.size(); }
    int onEvent(WiFiEventFuncCb cb) { handlers_.push_back(cb); return (int)handlers_.size(); }
    std::vector<WiFiEventFuncCb> handlers_;
};
extern WiFiClass WiFi;
