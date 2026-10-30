// =============================================================================
// sim_modules.cpp - simulator replacements for the clock modules that talk to
// hardware or the network directly (NTP, Wi-Fi, the diagnostic log, locks).
// Everything else - calendar, messages, drawing, settings - is the clock's
// own code, compiled unchanged. See sim/README.md.
// =============================================================================

#include <Arduino.h>
#include <atomic>
#include "NTPManager.h"
#include "WiFiManager.h"
#include "Location.h"
#include "DiagnosticLog.h"
#include "EventsLock.h"
#include "DisplayLock.h"
#include "NTPLock.h"
#include "DebugTime.h"
#include "sim_platform.h"

// Version.h (on the clock: src/Version.cpp, written by scripts/gen_version.py).
#include "Version.h"
const char FIRMWARE_VERSION[] = "sim";

// DebugTime.h's globals (normally in NTPManager.cpp).
std::atomic<int64_t> g_debugTimeOffsetSec{0};
std::atomic<bool> g_timeSyncedThisBoot{false};

// ── NTP: like the clock, no time until the first sync (Sim::ntpSyncMs after
//    "power on"); from then on Sim::setUtc() + elapsed ────────────────────────
static time_t g_utcAtZero = 0;          // UTC when simulated millis() was 0
void Sim::setUtc(time_t utc) { g_utcAtZero = utc - (time_t)(millis() / 1000); }
time_t Sim::utcNow() { return g_utcAtZero + (time_t)(millis() / 1000); }

time_t sim_time(time_t* out) {
    time_t t = millis() >= Sim::ntpSyncMs ? Sim::utcNow() : (time_t)(millis() / 1000);
    if (out) *out = t;
    return t;
}

namespace NTPManager {
static bool synced() {
    bool s = millis() >= Sim::ntpSyncMs;
    if (s) g_timeSyncedThisBoot = true;
    return s;
}
void begin(const char*, const char* tzPOSIX) { Sim::setTimezone(tzPOSIX ? tzPOSIX : "UTC0"); }
void loop() { synced(); }
void setTimezone(const char* tzPOSIX) { Sim::setTimezone(tzPOSIX); }
void setDebugOffset(int64_t offset) { g_debugTimeOffsetSec = offset; }
// Before the first sync the clock's own time counts from 1970 at power on.
time_t getTime() { return synced() ? Sim::utcNow() + g_debugTimeOffsetSec.load() : (time_t)(millis() / 1000); }
bool getDateTime(DateTime& out) {
    if (!synced()) return false;
    time_t t = getTime();
    struct tm info;
    localtime_r(&t, &info);
    out.year = info.tm_year + 1900; out.month = info.tm_mon + 1; out.day = info.tm_mday;
    out.hour = info.tm_hour; out.minute = info.tm_min; out.second = info.tm_sec;
    out.isDST = info.tm_isdst > 0;
    return true;
}
time_t getLocalEpoch() { time_t t = getTime(); return t + Sim::utcOffsetAt(t); }
void forceSync() {}
bool isSynced() { return synced(); }
unsigned long millisSinceSync() { return 0; }
} // namespace NTPManager

// ── Wi-Fi: connected from Sim::wifiUpMs (see WiFi.h). Like the clock, its
//    own access point starts together with the connection; no setup portal,
//    so nothing of its own to report on screen ──────────────────────────────
static const AppConfig* s_wifiCfg = nullptr;
void WiFiManager::begin(AppConfig& cfg) { s_wifiCfg = &cfg; }
void WiFiManager::loop() {}
// --setup-mode: the clock's setup screen (no network known, its setup access
// point) - the same line as WiFiManager.cpp's AP_PORTAL case.
bool g_simSetupMode = false;
String WiFiManager::getStatusMessage() {
    if (!g_simSetupMode) return String();
    return "Setup: join Wi-Fi " + String(AP_SSID_PREFIX) + "Setup - password " + String(AP_PASSWORD_DEFAULT) + " - open 192.168.4.1";
}
// The first known network, as if it were the strongest (no scan simulated).
String WiFiManager::joiningSsid() {
    for (int i = 0; s_wifiCfg && i < WIFI_NETWORKS; i++) if (wifiSsidAt(*s_wifiCfg, i)[0]) return wifiSsidAt(*s_wifiCfg, i);
    return String();
}
bool WiFiManager::isApActive() { return millis() >= Sim::wifiUpMs; }
uint32_t WiFiManager::getDisconnectCount() { return 0; }
uint32_t WiFiManager::getReconnectCount() { return 0; }
uint32_t WiFiManager::getApRebuildCount() { return 0; }
uint32_t WiFiManager::getLastReconnectChainMs() { return 0; }
// No network switching simulated: the place stays the one in the settings
// the simulator was given (the clock's active place).
int WiFiManager::connectedSlot() { return -1; }

// ── Location: no lookups or place searches in the simulator (no network) ─────
namespace Location {
void loop(AppConfig&) {}
bool applyLookup(AppConfig&, int, const String&) { return false; }
bool searchToStart(String&) { return false; }
void searchStarted() {}
void searchDone(int, const String&) {}
String searchUrl(const String&) { return String(); }
}

// ── Diagnostic log: printed (unless quiet), not stored ────────────────────────
void DiagnosticLog::begin() {}
void DiagnosticLog::record(const char* category, const char* fmt, ...) {
    if (Sim::quiet) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    fprintf(stderr, "[DiagnosticLog] [%s] %s\n", category, buf);
}
void DiagnosticLog::sendTo(AsyncWebServerRequest*) {}
void DiagnosticLog::loop() {}
void DiagnosticLog::flushNow() {}
void DiagnosticLog::discardPending() {}
// Same streak logic as the clock (DiagnosticLog.cpp).
void DiagnosticLog::Streak::fail(const char* category, const char* what, const char* detail) {
    if (++failed == after) record(category, "%s failing: %s", what, detail ? detail : "");
}
void DiagnosticLog::Streak::ok(const char* category, const char* what) {
    if (failed >= after) record(category, "%s works again after %u failed attempt(s)", what, (unsigned)failed);
    failed = 0;
}

// ── Locks: the simulator's main loop is the only drawer; the download task ────
//    touches nothing these locks protect.
void EventsLock::begin() {} void EventsLock::take() {} void EventsLock::give() {}
void DisplayLock::begin() {} void DisplayLock::take() {} void DisplayLock::give() {}
void NTPLock::begin() {} void NTPLock::take() {} void NTPLock::give() {}
