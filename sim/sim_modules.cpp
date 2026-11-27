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
#include "Storage.h"
#include "Location.h"
#include "Texts.h"
#include "DiagnosticLog.h"
#include "EventsLock.h"
#include "DisplayLock.h"
#include "NTPLock.h"
#include "DebugTime.h"
#include "sim_platform.h"

// The ESP32's memory and restart (stubs/Arduino.h): fixed numbers here; the
// full simulator (sim_clock_main.cpp) has the clock's own.
uint32_t EspClass::getFreeHeap() { return 250000; }
uint32_t EspClass::getMaxAllocHeap() { return 150000; }
uint32_t EspClass::getMinFreeHeap() { return 200000; }
uint32_t EspClass::getPsramSize() { return 8u * 1024u * 1024u; }
uint32_t EspClass::getFreePsram() { return 7u * 1024u * 1024u; }
void EspClass::restart() { std::exit(0); }

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
void begin(const char* const*, int, const char* tzPOSIX) { Sim::setTimezone(tzPOSIX ? tzPOSIX : "UTC0"); }
void setServers(const char* const*, int) {}
void testStart(const char*) {}
void testResult(int& state, String& server, String& detail) { state = 0; server = ""; detail = ""; }
String currentServer() { return NTP_SERVER_1; }
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
// One moment per pass (NTPManager.h): here simulated time moves only between passes
// (Sim::advanceMillis() after endPass()) - a pass is one moment already.
void beginPass() {}
void endPass() {}
void forceSync() {}
bool isSynced() { return synced(); }
int64_t msSinceSync() { return Sim::ntpSilentMs; }
void serviceTimeSeen() {}
void serviceTroubleFile() {}
int troubleList(TroubleInfo*, int) { return 0; }
bool hasRefusedServer() { return false; }
void setCountryPool(const char*) {}
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
    return Texts::fill(Texts::setupLine, String(AP_SSID_PREFIX) + "Setup", WiFiManager::apPassword());
}
// The first known network, as if it were the strongest (no scan simulated).
String WiFiManager::joiningSsid() {
    for (int i = 0; s_wifiCfg && i < WIFI_NETWORKS; i++) if (wifiSsidAt(*s_wifiCfg, i)[0]) return wifiSsidAt(*s_wifiCfg, i);
    return String();
}
bool WiFiManager::isApActive() { return millis() >= Sim::wifiUpMs; }
int64_t WiFiManager::offlineForMs() { return millis() >= Sim::wifiUpMs ? 0 : (int64_t)millis(); }
uint32_t WiFiManager::getDisconnectCount() { return 0; }
uint32_t WiFiManager::getReconnectCount() { return 0; }
uint32_t WiFiManager::getApRebuildCount() { return 0; }
uint32_t WiFiManager::getLastReconnectChainMs() { return 0; }
// No network switching simulated: the place stays the one in the settings
// the simulator was given (the clock's active place).
int WiFiManager::connectedSlot() { return -1; }
String WiFiManager::ownWifiName() { return s_wifiCfg ? String(s_wifiCfg->projectName).substring(0, 32) : String(); }
// As the clock's (WiFiManager.cpp): the one in its settings; none there - the build's, else one of its
// own (a fixed stand-in here: the real one is random and saved, the screen simulator saves nothing).
const char* WiFiManager::apPassword() {
    if (s_wifiCfg && !Storage::apPasswordProblem(s_wifiCfg->apPassword).length()) return s_wifiCfg->apPassword;
    return AP_PASSWORD_DEFAULT[0] ? AP_PASSWORD_DEFAULT : "abcd2345";
}

// ── Location: no lookups or place searches in the simulator (no network) ─────
namespace Location {
void loop(AppConfig&) {}
bool applyLookup(AppConfig&, int, const String&, uint32_t) { return false; }
uint32_t userSets(int) { return 0; }
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
void DiagnosticLog::sendTo(AsyncWebServerRequest*, bool) {}
size_t DiagnosticLog::growthLeft() { return 0; }
void DiagnosticLog::loop() {}
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

// Updater isn't in the screen simulator: no update ever failed here.
#include "Updater.h"
String Updater::failedUpdate() { return ""; }
String Updater::failedUpdateOnScreen() { return ""; }
// OnlineUpdate isn't in the screen simulator: nothing to fetch.
#include "OnlineUpdate.h"
#include "Texts.h"
void OnlineUpdate::runInTask() {}
bool OnlineUpdate::feedMissingOnScreen() { return false; }   // no feed asked here (web_ui online_update tests it)
