// =============================================================================
// sim_wifi.cpp - the full simulator's Wi-Fi driver (jcclock): the networks
// around the clock ("the world"), joining, scanning, the clock's own access
// point. The clock's WiFiManager.cpp runs on top of it unchanged.
//
// The world (<state>/world.json, or made from the clock's known networks at
// the first start; changeable while running - sim/README.md, control port):
//   {"networks": [{"ssid": "...", "password": "...", "ip": "<clock-ip>",
//     "internet": true, "up": true, "rssi": -62, "channel": 9, "bssid": "..."}]}
// One entry = one access point; several with the same name = a mesh. "ip" is
// the address the clock gets there; "up": false = switched off.
// =============================================================================

#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <deque>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include "sim_clock.h"

namespace {
struct Ap { std::string ssid, password, ip, bssid; bool internet = true, up = true; int rssi = -60, channel = 6; };
std::vector<Ap> g_world;
std::string g_worldPath;

struct Pending { WiFiEvent_t ev; WiFiEventInfo_t info; unsigned long at; };
std::deque<Pending> g_events;

wifi_mode_t g_mode = WIFI_OFF;
wl_status_t g_status = WL_IDLE_STATUS;
int g_joined = -1;                 // index into g_world while connected
bool g_ipUp = false;
// A connect under way: its outcome is decided when it is due.
bool g_connecting = false; unsigned long g_connectDueMs = 0; std::string g_wantSsid, g_wantPass;
bool g_apOn = false; std::string g_apSsid;
// The one shared scan result list (as the driver has).
bool g_scanRunning = false; unsigned long g_scanDueMs = 0; std::string g_scanOnly;
std::vector<int> g_scan; bool g_scanHave = false;

// Time a join takes, ms - from the clock's log (2026-11-05: "joining" to
// "reconnected" 1.6-4.1 s), widened as sim_platform.cpp does.
const Sim::Range JOIN_MS{1100, 6200};
const unsigned long LINK_TO_IP_MS = 300;

void queue(WiFiEvent_t ev, const WiFiEventInfo_t& info, unsigned long at) { g_events.push_back({ev, info, at}); }

void parseBssid(const std::string& s, uint8_t out[6]) {
    unsigned v[6] = {0};
    sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
}

void dropConnection(uint8_t reason) {
    if (g_status == WL_CONNECTED || g_joined >= 0) {
        WiFiEventInfo_t info{}; info.wifi_sta_disconnected.reason = reason;
        queue(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info, millis());
    }
    g_joined = -1; g_ipUp = false;
    g_status = reason == 200 ? WL_CONNECTION_LOST : WL_DISCONNECTED;
}

void decideConnect() {
    g_connecting = false;
    int best = -1, nameSeen = 0;
    for (size_t i = 0; i < g_world.size(); i++) {
        const Ap& a = g_world[i];
        if (!a.up || a.ssid != g_wantSsid) continue;
        nameSeen++;
        if (a.password != g_wantPass) continue;
        if (best < 0 || a.rssi > g_world[best].rssi) best = (int)i;   // WIFI_CONNECT_AP_BY_SIGNAL
    }
    WiFiEventInfo_t info{};
    if (best < 0) {
        info.wifi_sta_disconnected.reason = nameSeen ? 202 : 201;   // AUTH_FAIL / NO_AP_FOUND
        g_status = nameSeen ? WL_CONNECT_FAILED : WL_NO_SSID_AVAIL;
        queue(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info, millis());
        return;
    }
    g_joined = best;
    const Ap& a = g_world[best];
    parseBssid(a.bssid, info.wifi_sta_connected.bssid);
    info.wifi_sta_connected.channel = (uint8_t)a.channel;
    queue(ARDUINO_EVENT_WIFI_STA_CONNECTED, info, millis());
    WiFiEventInfo_t ipInfo{};
    ipInfo.got_ip.ip_info.ip.addr = IPAddress::parse(a.ip.c_str()).addr();
    queue(ARDUINO_EVENT_WIFI_STA_GOT_IP, ipInfo, millis() + LINK_TO_IP_MS);
}

std::string defaultBssid(int i) { char b[24]; snprintf(b, sizeof(b), "5A:1E:00:00:%02X:%02X", (i * 37) & 0xFF, (i * 91 + 17) & 0xFF); return b; }
} // namespace

// ── The world ─────────────────────────────────────────────────────────────────
static bool parseWorld(const std::string& json, std::vector<Ap>& out, std::string& err) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) { err = "not JSON"; return false; }
    out.clear();
    int i = 0;
    for (JsonObject n : doc["networks"].as<JsonArray>()) {
        Ap a;
        char defIp[20]; snprintf(defIp, sizeof(defIp), "192.168.%d.240", 31 + i * 10);   // as simDefaultWorld() makes them
        a.ssid = n["ssid"] | ""; a.password = n["password"] | ""; a.ip = n["ip"] | (const char*)defIp;
        a.internet = n["internet"] | true; a.up = n["up"] | true; a.rssi = n["rssi"] | -60; a.channel = n["channel"] | 6;
        a.bssid = n["bssid"] | defaultBssid(i).c_str();
        if (!a.ssid.empty()) out.push_back(a);
        i++;
    }
    return true;
}
std::string Sim::worldJson() {
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (auto& a : g_world) {
        JsonObject n = arr.add<JsonObject>();
        n["ssid"] = a.ssid; n["password"] = a.password; n["ip"] = a.ip; n["internet"] = a.internet;
        n["up"] = a.up; n["rssi"] = a.rssi; n["channel"] = a.channel; n["bssid"] = a.bssid;
    }
    std::string s; serializeJsonPretty(doc, s);
    return s;
}
bool Sim::setWorldJson(const std::string& json, std::string& err) {
    std::vector<Ap> w;
    if (!parseWorld(json, w, err)) return false;
    std::string joinedBssid = g_joined >= 0 ? g_world[g_joined].bssid : "";
    g_world = w;
    if (!g_worldPath.empty()) { std::ofstream f(g_worldPath, std::ios::binary); f << worldJson(); }
    // The access point we're on: still there, else the connection drops.
    if (g_joined >= 0) {
        int again = -1;
        for (size_t i = 0; i < g_world.size(); i++) if (g_world[i].bssid == joinedBssid && g_world[i].up) again = (int)i;
        if (again < 0) dropConnection(200);   // BEACON_TIMEOUT, as when an access point disappears
        else g_joined = again;
    }
    return true;
}
void Sim::loadWorld(const std::string& path) {
    g_worldPath = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string err;
    if (!parseWorld(ss.str(), g_world, err)) fprintf(stderr, "[sim] %s: %s\n", path.c_str(), err.c_str());
}

bool Sim::staUp() { return g_status == WL_CONNECTED && g_ipUp; }
bool Sim::apUp() { return g_apOn; }
bool Sim::wifiInternet() { return staUp() && g_joined >= 0 && g_world[g_joined].internet; }
std::string Sim::staIpString() { return staUp() ? g_world[g_joined].ip : std::string("0.0.0.0"); }

void Sim::wifiPumpEvents() {
    unsigned long now = millis();
    if (g_connecting && now >= g_connectDueMs) decideConnect();
    if (g_scanRunning && now >= g_scanDueMs) {
        g_scanRunning = false; g_scanHave = true; g_scan.clear();
        for (size_t i = 0; i < g_world.size(); i++)
            if (g_world[i].up && (g_scanOnly.empty() || g_world[i].ssid == g_scanOnly)) g_scan.push_back((int)i);
    }
    while (!g_events.empty() && g_events.front().at <= now) {
        Pending p = g_events.front(); g_events.pop_front();
        if (p.ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            if (g_joined < 0) continue;   // dropped meanwhile
            g_ipUp = true; g_status = WL_CONNECTED;
        }
        for (auto& h : WiFi.handlers_) h(p.ev, p.info);
    }
}

// ── The driver API the clock uses ─────────────────────────────────────────────
wl_status_t WiFiClass::begin(const char* ssid, const char* pass) {
    if (g_joined >= 0) dropConnection(8);   // ASSOC_LEAVE: leaving for the new attempt
    g_wantSsid = ssid ? ssid : ""; g_wantPass = pass ? pass : "";
    g_connecting = true;
    g_connectDueMs = millis() + Sim::drawTiming(JOIN_MS, "join");
    if (!(g_mode & WIFI_STA)) g_mode = (wifi_mode_t)(g_mode | WIFI_STA);
    g_status = WL_DISCONNECTED;
    return g_status;
}
bool WiFiClass::disconnect(bool, bool) {
    g_connecting = false;
    if (g_joined >= 0) dropConnection(8);
    else g_status = WL_DISCONNECTED;
    return true;
}
wl_status_t WiFiClass::status() { return g_status; }
IPAddress WiFiClass::localIP() { return Sim::staUp() ? IPAddress::parse(g_world[g_joined].ip.c_str()) : IPAddress(0, 0, 0, 0); }
String WiFiClass::SSID() { return g_joined >= 0 ? String(g_world[g_joined].ssid.c_str()) : String(); }
int8_t WiFiClass::RSSI() { return g_joined >= 0 ? (int8_t)g_world[g_joined].rssi : 0; }
String WiFiClass::BSSIDstr() { return g_joined >= 0 ? String(g_world[g_joined].bssid.c_str()) : String(); }
int32_t WiFiClass::channel() { return g_joined >= 0 ? g_world[g_joined].channel : 0; }

bool WiFiClass::mode(wifi_mode_t m) {
    g_mode = m;
    if (!(m & WIFI_AP)) g_apOn = false;
    if (!(m & WIFI_STA)) { g_connecting = false; if (g_joined >= 0) dropConnection(8); }
    return true;
}
wifi_mode_t WiFiClass::getMode() { return g_mode; }
bool WiFiClass::softAP(const char* ssid, const char*, int, int, int) {
    g_mode = (wifi_mode_t)(g_mode | WIFI_AP);
    g_apOn = true; g_apSsid = ssid ? ssid : "";
    return true;
}
bool WiFiClass::softAPdisconnect(bool) { g_apOn = false; g_mode = (wifi_mode_t)(g_mode & ~WIFI_AP); return true; }
IPAddress WiFiClass::softAPIP() { return g_apOn ? IPAddress(192, 168, 4, 1) : IPAddress(0, 0, 0, 0); }

int16_t WiFiClass::scanNetworks(bool async, bool, bool, uint32_t msPerChannel, uint8_t, const char* ssid, const uint8_t*) {
    if (g_connecting) return WIFI_SCAN_FAILED;   // ESP_ERR_WIFI_STATE while a connect is under way
    g_scanRunning = true; g_scanHave = false; g_scan.clear();
    g_scanOnly = ssid ? ssid : "";
    g_scanDueMs = millis() + msPerChannel * 13;   // 13 channels
    if (!async) { g_scanDueMs = millis(); Sim::wifiPumpEvents(); return (int16_t)g_scan.size(); }
    return WIFI_SCAN_RUNNING;
}
int16_t WiFiClass::scanComplete() {
    if (g_scanRunning) return WIFI_SCAN_RUNNING;
    return g_scanHave ? (int16_t)g_scan.size() : WIFI_SCAN_FAILED;
}
void WiFiClass::scanDelete() { g_scanHave = false; g_scan.clear(); }
esp_err_t esp_wifi_scan_stop() { g_scanRunning = false; g_scanHave = false; g_scan.clear(); return ESP_OK; }
static const Ap* scanAt(uint8_t i) { return i < g_scan.size() && g_scan[i] < (int)g_world.size() ? &g_world[g_scan[i]] : nullptr; }
String WiFiClass::SSID(uint8_t i) { auto a = scanAt(i); return a ? String(a->ssid.c_str()) : String(); }
int32_t WiFiClass::RSSI(uint8_t i) { auto a = scanAt(i); return a ? a->rssi : 0; }
String WiFiClass::BSSIDstr(uint8_t i) { auto a = scanAt(i); return a ? String(a->bssid.c_str()) : String(); }
int32_t WiFiClass::channel(uint8_t i) { auto a = scanAt(i); return a ? a->channel : 0; }
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i) { auto a = scanAt(i); return a && a->password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK; }

// A world from the clock's known networks, when there is none yet: every
// known network around, with its saved password and internet.
void simDefaultWorld(const char* const ssids[], const char* const passwords[], int n) {
    if (!g_world.empty()) return;
    for (int i = 0; i < n; i++) {
        if (!ssids[i] || !ssids[i][0]) continue;
        Ap a; a.ssid = ssids[i]; a.password = passwords[i] ? passwords[i] : "";
        char ip[20]; snprintf(ip, sizeof(ip), "192.168.%d.240", 31 + i * 10);
        a.ip = ip; a.rssi = -55 - i * 6; a.channel = 1 + (i * 5) % 11; a.bssid = defaultBssid(i);
        g_world.push_back(a);
    }
    if (!g_worldPath.empty()) { std::ofstream f(g_worldPath, std::ios::binary); f << Sim::worldJson(); }
}
