#include "Location.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Storage.h"
#include "Calendar.h"
#include "WiFiManager.h"
#include "NTPManager.h"
#include "StringTools.h"
#include "DiagnosticLog.h"
#include "LogLevel.h"
#include "Texts.h"

namespace Location {

static int s_activeSlot = -1;              // the network whose place is active (-1: none yet this boot)
// A place chosen in the page, waiting for loop() (queuePlace() runs on the
// web server's task). One at a time; the mutex covers the copy.
static SemaphoreHandle_t s_queueMutex = nullptr;
static Place s_queued;
static std::atomic<int> s_queuedSlot{-1};
static std::atomic<uint32_t> s_forgetMask{0};   // bit i: forget network i's place
static std::atomic<uint32_t> s_userSets[WIFI_NETWORKS];   // userSets(): places the user set or confirmed

uint32_t userSets(int slot) { return slot >= 0 && slot < WIFI_NETWORKS ? s_userSets[slot].load() : 0; }

void forgetPlace(int slot) { if (slot >= 0 && slot < WIFI_NETWORKS) s_forgetMask |= (1u << slot); }

bool isIsrael(const Place& p) {
    return strcmp(p.country, "Israel") == 0 || strcmp(p.tzid, "Asia/Jerusalem") == 0;
}

String posixFor(const char* tzid) {
    if (!tzid || !*tzid) return "";
    File f = LittleFS.open(FS_TZ_TABLE, "r");
    if (!f) return "";
    String want = String(tzid) + "\t";
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.startsWith(want)) { f.close(); line.trim(); return line.substring(want.length()); }
    }
    f.close();
    return "";
}

// The rule the clock's time runs on for place p: Israel's built-in one (the
// table can't express "Friday before the last Sunday"), the table's, or -
// for a zone not in the table - its fixed offset (no summer time).
static void ruleFor(const Place& p, char* out, size_t n) {
    if (isIsrael(p)) { StringTools::utf8Cut(out, n, DEFAULT_TZ_POSIX, n - 1); return; }
    if (p.tzPOSIX[0]) { StringTools::utf8Cut(out, n, p.tzPOSIX, n - 1); return; }
    int w = -p.offsetSec, a = abs(w), h = a / 3600, m = (a % 3600) / 60;   // POSIX offsets are west-positive
    int e = p.offsetSec, ea = abs(e);
    snprintf(out, n, "<%c%02d%02d>%s%d%s", e < 0 ? '-' : '+', ea / 3600, (ea % 3600) / 60, w < 0 ? "-" : "", h,
             m ? (String(":") + (m < 10 ? "0" : "") + String(m)).c_str() : "");
}

// Makes place `slot` the active one. Saves and re-downloads the calendar only
// when something actually changed (normally: never, on a reconnect). True =
// changed (and saved). byUser false: the clock's own doing - saved when the
// screen stands still (Storage::saveConfigSoon()).
static bool activate(AppConfig& cfg, int slot, bool byUser) {
    const Place& p = cfg.places[slot];
    // Always update the country pool - s_countryPool starts "" each boot, so this
    // runs even when the city/timezone are already correct (the most common restart case).
    NTPManager::setCountryPool(p.countryCode);
    char rule[sizeof(cfg.tzPOSIX)];
    ruleFor(p, rule, sizeof(rule));
    bool moved = strcmp(cfg.locationCity, p.city) || strcmp(cfg.locationCountry, p.country) ||
                 cfg.latitude != p.lat || cfg.longitude != p.lon || strcmp(cfg.tzid, p.tzid);
    bool newRule = strcmp(cfg.tzPOSIX, rule) != 0;
    if (!moved && !newRule) return false;
    StringTools::utf8Copy(cfg.locationCity, p.city);
    StringTools::utf8Copy(cfg.locationCountry, p.country);
    cfg.latitude = p.lat;
    cfg.longitude = p.lon;
    StringTools::utf8Copy(cfg.tzid, p.tzid);
    StringTools::utf8Copy(cfg.tzPOSIX, rule);
    cfg.inIsrael = isIsrael(p);
    if (newRule) NTPManager::setTimezone(cfg.tzPOSIX);
    if (byUser) Storage::saveConfig(cfg); else Storage::saveConfigSoon();
    if (moved) Calendar::forceRefresh();   // another place: other times, maybe other holidays
    DiagnosticLog::record("Location", "network %d -> %s, %s (%s, rule %s)", slot + 1, p.city, p.country, p.tzid, cfg.tzPOSIX);
    return true;
}

bool applyLookup(AppConfig& cfg, int slot, const String& payload, uint32_t asked) {
    if (slot < 0 || slot >= WIFI_NETWORKS) return false;
    if (s_userSets[slot] != asked) {   // set or confirmed by the user while it was looked up: theirs stays
        DiagnosticLog::record("Location", "network %d: a lookup came after the place was set - the place set is kept", slot + 1);
        return true;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload) || doc["status"] != "success") return false;
    Place p = {};
    p.set = true;
    p.confirmed = false;
    StringTools::utf8Copy(p.city,        doc["city"]        | "");
    StringTools::utf8Copy(p.country,     doc["country"]     | "");
    StringTools::utf8Copy(p.countryCode, doc["countryCode"] | "");
    for (char* c = p.countryCode; *c; c++) *c = StringTools::asciiToLower(*c);
    StringTools::utf8Copy(p.tzid,        doc["timezone"]    | "");
    p.lat = doc["lat"] | 0.0f;
    p.lon = doc["lon"] | 0.0f;
    p.offsetSec = doc["offset"] | 0;
    StringTools::utf8Copy(p.tzPOSIX, posixFor(p.tzid).c_str());
    if (!p.city[0] || !p.tzid[0]) return false;
    cfg.places[slot] = p;
    DiagnosticLog::record("Location", Texts::placeLookedUp, slot + 1, p.city, p.country, p.tzid);
    if (slot != s_activeSlot || !activate(cfg, slot, false)) Storage::saveConfigSoon();   // one save (see loop())
    return true;
}

bool queuePlace(int slot, const Place& in, String& error) {
    if (slot < 0 || slot >= WIFI_NETWORKS) { error = "No such network"; return false; }
    if (!in.city[0] || !in.tzid[0] || in.lat < -90 || in.lat > 90 || in.lon < -180 || in.lon > 180) { error = "Incomplete place"; return false; }
    Place p = in;
    p.set = true;
    p.confirmed = true;
    StringTools::utf8Copy(p.tzPOSIX, posixFor(p.tzid).c_str());
    if (!s_queueMutex) s_queueMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_queueMutex, portMAX_DELAY);
    s_queued = p;
    s_queuedSlot = slot;
    s_userSets[slot]++;   // a lookup asked before this no longer counts (applyLookup)
    xSemaphoreGive(s_queueMutex);
    return true;
}

// ── Place search ──
enum class Search { Idle, Waiting, Running, Done, Error };
static SemaphoreHandle_t s_searchMutex = xSemaphoreCreateMutex();
static Search s_search = Search::Idle;
static String s_searchQuery, s_searchResults = "[]";

void requestSearch(const String& query) {
    xSemaphoreTake(s_searchMutex, portMAX_DELAY);
    s_searchQuery = query;
    s_searchResults = "[]";
    s_search = Search::Waiting;
    xSemaphoreGive(s_searchMutex);
}

String searchStatus() {
    static const char* names[] = { "idle", "waiting", "waiting", "done", "error" };
    xSemaphoreTake(s_searchMutex, portMAX_DELAY);
    String out = String("{\"state\":\"") + names[(int)s_search] + "\",\"results\":" + s_searchResults + "}";
    xSemaphoreGive(s_searchMutex);
    return out;
}

bool searchToStart(String& query) {
    xSemaphoreTake(s_searchMutex, portMAX_DELAY);
    bool waiting = s_search == Search::Waiting;
    if (waiting) query = s_searchQuery;
    xSemaphoreGive(s_searchMutex);
    return waiting;
}

void searchStarted() {
    xSemaphoreTake(s_searchMutex, portMAX_DELAY);
    if (s_search == Search::Waiting) s_search = Search::Running;
    xSemaphoreGive(s_searchMutex);
}

String searchUrl(const String& query) {
    String q;
    bool hebrew = false;
    for (size_t i = 0; i < query.length(); i++) {   // URL-encode (UTF-8 bytes as %XX)
        uint8_t c = query[i];
        if (c == 0xD7) hebrew = true;               // the lead byte of every Hebrew letter (U+05C0..U+05FF)
        if (StringTools::asciiAlnum((char)c) || c == '-' || c == '.' || c == '_') q += (char)c;
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); q += b; }
    }
    // The service finds a name only in the language it is asked in: "תל אביב" with language=en
    // found nothing - a city typed in Hebrew was always "Not found" (2026-11-18). Hebrew: asked in
    // Hebrew (the names come back in Hebrew).
    return String("https://geocoding-api.open-meteo.com/v1/search?count=8&language=") + (hebrew ? "he" : "en") +
           "&format=json&name=" + q;
}

// The service's answer -> a short list for the page: name, region, country,
// coordinates, zone.
void searchDone(int httpCode, const String& payload) {
    String list = "[]";
    bool ok = false;
    if (httpCode == 200) {
        JsonDocument in;
        if (!deserializeJson(in, payload)) {
            JsonDocument out;
            JsonArray arr = out.to<JsonArray>();
            for (JsonObject r : in["results"].as<JsonArray>()) {
                JsonObject o = arr.add<JsonObject>();
                o["name"] = r["name"]; o["region"] = r["admin1"]; o["country"] = r["country"];
                o["lat"] = r["latitude"]; o["lon"] = r["longitude"]; o["tzid"] = r["timezone"];
            }
            list = "";
            serializeJson(out, list);
            ok = true;
        }
    }
    xSemaphoreTake(s_searchMutex, portMAX_DELAY);
    s_searchResults = list;
    s_search = ok ? Search::Done : Search::Error;
    xSemaphoreGive(s_searchMutex);
}

void loop(AppConfig& cfg) {
    uint32_t forget = s_forgetMask.exchange(0);
    if (forget) {
        for (int i = 0; i < WIFI_NETWORKS; i++) {
            if (!(forget & (1u << i)) || !cfg.places[i].set) continue;
            memset(&cfg.places[i], 0, sizeof(Place));
            if (i == s_activeSlot) s_activeSlot = -1;   // re-decided when (re)joined
            DiagnosticLog::record("Location", "network %d's place forgotten (network changed)", i + 1);
        }
        Storage::saveConfig(cfg);
    }

    int q = s_queuedSlot.exchange(-1);
    if (q >= 0 && s_queueMutex) {
        xSemaphoreTake(s_queueMutex, portMAX_DELAY);
        cfg.places[q] = s_queued;
        xSemaphoreGive(s_queueMutex);
        DiagnosticLog::record("Location", "network %d set to %s, %s (%s)", q + 1, cfg.places[q].city, cfg.places[q].country, cfg.places[q].tzid);
        // One save: activate() saves when the active place changes (a flash
        // write pauses the screen - once is enough).
        if (q != s_activeSlot || !activate(cfg, q, true)) Storage::saveConfig(cfg);
    }

    // Keep the place while disconnected; act when a network is joined.
    int slot = WiFiManager::connectedSlot();
    if (slot < 0 || slot == s_activeSlot) return;
    s_activeSlot = slot;
    if (cfg.places[slot].set) activate(cfg, slot, false);
    else Calendar::requestLookup(slot);
}

} // namespace Location
