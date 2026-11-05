// =============================================================================
// WebUI.cpp
// JewishClock - Async web server: REST API + static file serving.
//
// All endpoints are available in BOTH STA mode and AP setup mode, because
// the same AsyncWebServer instance handles requests regardless of Wi-Fi mode.
// This means backup restore works in AP mode: upload a backup to restore
// Wi-Fi credentials + settings without typing anything.
// =============================================================================

#include "WebUI.h"
#include "Config.h"
#include "Version.h"
#include "Storage.h"
#include "Display.h"
#include "JewishDateEngine.h"
#include "OnScreenElement.h"
#include "Calendar.h"
#include "GeolocationService.h"
#include "NTPManager.h"
#include "WiFiManager.h"
#include "Location.h"
#include "MessageAggregator.h"
#include "StringTools.h"
#include "AmbientLight.h"
#include "BrightnessManager.h"
#include "DiagnosticLog.h"
#include "FlashStats.h"
#include "LogLevel.h"
#include "EventsLock.h"
#include "Updater.h"
#include "ImageLibrary.h"
#include <esp_heap_caps.h>
#include <map>

#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <atomic>
#include "mdns.h"  // ESP-IDF direct access for removeService


// === TUNABLES ===  (rule: see Config.h header)

// mDNS tiered upkeep - see the Tier 1 / Tier 2 blocks in WebUI::loop().
static const unsigned long MDNS_REFRESH_MS        = 20UL * 60UL * 1000UL;  // Tier 1 keep-alive
static const unsigned long MDNS_STALE_GRACE_MS    = 6UL * 60UL * 1000UL;
static const unsigned long MDNS_STALE_MS          = MDNS_REFRESH_MS + MDNS_STALE_GRACE_MS; // Tier 1 presumed failed -> Tier 2
static const unsigned long MDNS_WATCHDOG_CHECK_MS = 60UL * 1000UL;         // Tier 2 health check
static const uint8_t       MDNS_FORCE_RETRY_ATTEMPTS = 3;                  // see g_mdnsInitAttempts
static const unsigned long MDNS_RESTART_PAUSE_MS  = 250;                   // MDNS.end() -> begin(), see performMdnsInit()

// Diagnostic heartbeat line in the DiagnosticLog.
static const unsigned long HEARTBEAT_LOG_MS = 30UL * 60UL * 1000UL;

// Weak-signal episodes - see that block in WebUI::loop().
static const int           WIFI_RSSI_WEAK_DBM      = -85;
static const int           WIFI_RSSI_RECOVERED_DBM = -80;
static const unsigned long WIFI_RSSI_SAMPLE_MS     = 5000;

// /api/status's own serial summary line, at most this often.
static const unsigned long STATUS_LOG_INTERVAL_MS = 60UL * 1000UL;

// Dev helper: re-read the front end's build time this often.
static const unsigned long FE_BUILD_TIME_REFRESH_MS = 60UL * 1000UL;

// Delay between a handler's reply and the restart it requested, so the
// reply is actually sent first (see scheduleRestart()).
static const uint32_t RESTART_AFTER_REPLY_MS = 1500;

// Settings changes apply immediately (in RAM) but are written to flash only
// once nothing has changed for this long - see markConfigChanged().
static const uint32_t CONFIG_SAVE_QUIET_MS = 2000;

// Free-PSRAM limits for adding events (the real operational limit - see
// MAX_EVENTS in Config.h). PSRAM is shared with everything else on the
// device, so something unrelated eating into it should throttle new events
// just as much as events themselves would. Round, conservative numbers, not
// derived from a measurement - revisit if they turn out to bite in practice.
static const float EVENTS_PSRAM_WARN_PCT   = 20.0f;  // new events still accepted, but flagged
static const float EVENTS_PSRAM_REFUSE_PCT = 10.0f;  // new events rejected outright

// Largest picture file accepted at all - a sanity limit only; what really
// decides is the free space (ImageLibrary::hasRoomFor()). The form makes
// 72x96 (~14 KB); one still picture, no animations.
static const size_t IMAGE_MAX_BYTES = 64 * 1024;
// All replacements for the clock's own pictures together (they may be short
// animations - 2-4 of them; the user, 2026-10-21).
static const size_t CUSTOM_IMAGES_MAX_BYTES = 800 * 1024;


// ─── Static State ─────────────────────────────────────────────────────────────

AsyncWebServer *WebUI::g_server = nullptr;
static AppConfig *g_cfg = nullptr;
static EventList *g_events = nullptr;
static bool g_serverStarted = false;
static String g_feBuildTime = "Unknown";

// Restart requested by a handler, performed by loop() on the main task. A
// handler that called delay()+ESP.restart() itself blocked the AsyncTCP task
// that sends its queued reply, so the client never got the response and the
// Settings page reported "could not reach the device" even though the
// restore/forget had worked. 0 = nothing pending.
static std::atomic<uint32_t> g_restartAtMs{0};
static void scheduleRestart(uint32_t afterMs) {
    uint32_t at = millis() + afterMs;
    g_restartAtMs = at ? at : 1;
}

// Settings page changes are "live": each one is applied to g_cfg at once,
// but writing config.json on every change would mean dozens of flash writes
// per slider drag. So a change only marks the config dirty; loop() writes it
// once there's been CONFIG_SAVE_QUIET_MS of quiet, and anything that needs
// the file current (a restart, a backup download) flushes first. Cost: a
// power cut within those 2 s loses the last change.
static std::atomic<bool>     g_cfgDirty{false};
static std::atomic<uint32_t> g_cfgChangedAtMs{0};
static std::atomic<uint32_t> g_cfgChangesPending{0};
// Names of the settings changed since the last write, for the log line.
// Written on the AsyncTCP task, read+cleared on the main loop -> spinlock.
static char        g_cfgChangedKeys[120] = "";
static portMUX_TYPE g_cfgKeysMux = portMUX_INITIALIZER_UNLOCKED;
static void markConfigChanged(JsonObjectConst changed) {
    portENTER_CRITICAL(&g_cfgKeysMux);
    for (JsonPairConst kv : changed) {
        const char* k = kv.key().c_str();
        if (strstr(g_cfgChangedKeys, k)) continue;   // close enough for a log line
        size_t len = strlen(g_cfgChangedKeys);
        snprintf(g_cfgChangedKeys + len, sizeof(g_cfgChangedKeys) - len, "%s%s", len ? ", " : "", k);
    }
    portEXIT_CRITICAL(&g_cfgKeysMux);
    g_cfgChangedAtMs = millis();
    g_cfgChangesPending++;
    g_cfgDirty = true;   // last: a concurrent flush that already started re-marks nothing it hasn't seen
}
static void flushConfigSave(const char* why) {
    if (!g_cfgDirty.exchange(false)) return;
    uint32_t n = g_cfgChangesPending.exchange(0);
    char keys[sizeof(g_cfgChangedKeys)];
    portENTER_CRITICAL(&g_cfgKeysMux);
    strlcpy(keys, g_cfgChangedKeys, sizeof(keys));
    g_cfgChangedKeys[0] = 0;
    portEXIT_CRITICAL(&g_cfgKeysMux);
    Storage::saveConfig(*g_cfg);
    DiagnosticLog::record("CONFIG", "saved %s (%u change(s), %s)", keys, (unsigned)n, why);
}

// mDNS Lifecycle State
static bool g_mdnsRunning = false;
static unsigned long g_lastMdnsAliveAt = 0;   // Timestamp of last confirmed activity
static unsigned long g_lastTier1Refresh = 0;  // Last lightweight keep-alive
static unsigned long g_lastMdnsHealthCheckMs = 0; // Last Tier 2 mDNS health check (every MDNS_WATCHDOG_CHECK_MS) - not the ESP task watchdog
static unsigned long g_lastTier2At = 0;       // Timestamp of last full restart

// Deferred mDNS (re)init request - see startServer()'s comment and the
// "Reconnect-triggered fast path" block in loop(). 0 = nothing pending.
static unsigned long g_mdnsRecheckAtMs = 0;
static String        g_pendingMdnsHost;
// performMdnsInit() stopped mDNS; MDNS.begin() is due at this time (0 = not).
static unsigned long g_mdnsBeginAtMs = 0;
static String        g_mdnsBeginHost;

// How many times performMdnsInit() has actually run this boot. Used to force
// a real reinit on the first few attempts even when g_mdnsRunning claims
// true - MDNS.begin() can report success while not actually answering real
// network queries (see scheduleMdnsRecheck()'s comment), and this is the
// only way found so far to self-heal that without trusting a flag that's
// been observed to lie. Capped retries, not unconditional every time, so
// this can't reintroduce the original reconnect-storm thrashing under
// sustained poor signal once mDNS has had a genuine chance to settle
// (MDNS_FORCE_RETRY_ATTEMPTS).
static uint8_t g_mdnsInitAttempts = 0;

// ── Read-only diagnostic counters (networking-reliability investigation) ────
static uint32_t g_diagMdnsTier2FromServer   = 0; // performMdnsInit() calls from the reconnect fast path
static uint32_t g_diagMdnsTier2FromWatchdog = 0; // performMdnsInit() calls from the 60s Tier 2 health check (name kept: matches the /api JSON key)

// ─── mDNS Tiered Strategy ─────────────────────────────────────────────────────

/**
 * Tier 2 — Full recovery. Emergency-only path.
 * Tears down and rebuilds the entire mDNS daemon.
 * Only called on initial startup or when the Tier 2 health check confirms the daemon is dead.
 * NOT called during routine maintenance — doing so causes goodbye packet broadcast,
 * heap fragmentation, and a downtime window on every cycle.
 */
static void performMdnsInit(const char* mdnsHost) {
    CLOCK_LOG("WebUI", "[MdnsDiag] performMdnsInit ENTER, host='%s', localIP=%s, softAPIP=%s, mode=%d, attempt=%u",
              mdnsHost, WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str(),
              (int)WiFi.getMode(), g_mdnsInitAttempts + 1);
    if (WiFi.localIP() == IPAddress(0,0,0,0)) {
        CLOCK_LOG("WebUI", "[MdnsDiag] Skipping init, WiFi IP is 0.0.0.0");
        return;
    }
    if (g_mdnsInitAttempts < 255) g_mdnsInitAttempts++;
    MDNS.end();
    g_mdnsRunning = false;
    // The pause before begin() (stack stabilization) is waited out by loop(),
    // not here: a delay() here froze the screen ~310 ms on every connect
    // (2026-10-25, "SLOW STEP: WebUI::loop").
    g_mdnsBeginHost = mdnsHost;
    g_mdnsBeginAtMs = millis() + MDNS_RESTART_PAUSE_MS;
    if (!g_mdnsBeginAtMs) g_mdnsBeginAtMs = 1;
}

// The second half of performMdnsInit(), MDNS_RESTART_PAUSE_MS later.
static void finishMdnsInit() {
    const char* mdnsHost = g_mdnsBeginHost.c_str();
    if (WiFi.localIP() == IPAddress(0,0,0,0)) return;   // the watchdog tries again
    if (MDNS.begin(mdnsHost)) {
        // Explicitly set the instance name to ensure discovery across all OS types
        MDNS.setInstanceName(mdnsHost);
        MDNS.addService("http", "tcp", 80);
        g_mdnsRunning = true;
        g_lastMdnsAliveAt = millis();
        g_lastTier2At = millis();
        CLOCK_LOG("WebUI", "[MdnsDiag] MDNS.begin() OK -> http://%s.local (IP: %s)",
                  mdnsHost, WiFi.localIP().toString().c_str());
        DiagnosticLog::record("MDNS", "reinit OK host=%s attempt=%u", mdnsHost, g_mdnsInitAttempts);
    } else {
        g_mdnsRunning = false;
        CLOCK_LOG("WebUI", "[MdnsDiag] MDNS.begin() FAILED");
        DiagnosticLog::record("MDNS", "reinit FAILED host=%s attempt=%u", mdnsHost, g_mdnsInitAttempts);
    }
}

/**
 * Tier 1 — Lightweight keep-alive.
 * Updates a TXT record to force a gratuitous mDNS announcement. 
 * This refreshes network caches WITHOUT tearing down the daemon or tasks.
 */
static void refreshMdnsServices() {
    if (!g_mdnsRunning) {
        return;
    }
    DiagnosticLog::record("MDNS", "tier1 refresh fired");

    // Tier 1: Update the "refresh" TXT record.
    // This replaces the existing value for the key "refresh" (it does not add new ones).
    // The update forces a gratuitous mDNS announcement to the network.
    MDNS.addServiceTxt("http", "tcp", "refresh", String(millis()).c_str()); //
    // Update the alive timestamp so Tier 2 knows the system called the daemon.
    g_lastMdnsAliveAt = millis();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

// A page file as stored: plain, or gzipped (utils/web_files.py - the
// library's send() serves "<path>.gz" when "<path>" is missing). "" = neither.
static String webFilePath(const char* path)
{
    if (LittleFS.exists(path)) return path;
    String gz = String(path) + ".gz";
    return LittleFS.exists(gz) ? gz : String();
}

// Sends a page file. Gzipped: with an ETag (the gzip's CRC, as the library
// makes it), and an unchanged file answered 304 without a body. Not the
// library's send(LittleFS, path) for that: its 304 queued the headers
// without sending them - they left only at the network's next flush, 40-580
// ms later, slower than sending the whole file (measured 2026-11-04). The
// ETag is read once per file and kept: web files change only by uploadfs or
// an update, both of which restart the clock.
static void sendWebFile(AsyncWebServerRequest* req, const char* path, const char* contentType)
{
    struct Known { String gz, etag; };
    static std::map<String, Known> known;   // handlers all run on the one web task
    auto it = known.find(path);
    if (it == known.end()) {
        Known k;
        String gz = String(path) + ".gz";
        if (!LittleFS.exists(path) && LittleFS.exists(gz)) {
            File f = LittleFS.open(gz, "r");
            uint32_t crc = 0;
            if (f && f.size() > 8 && f.seek(f.size() - 8) && f.read((uint8_t*)&crc, 4) == 4) {
                char etag[12];
                snprintf(etag, sizeof(etag), "\"%08X\"", (unsigned)crc);
                k.gz = gz; k.etag = etag;
            }
            if (f) f.close();
        }
        it = known.emplace(path, k).first;
    }
    const Known& k = it->second;
    if (!k.gz.length()) { req->send(LittleFS, path, contentType); return; }   // plain (or missing: 404)
    AsyncWebServerResponse* r;
    if (req->hasHeader("If-None-Match") && req->getHeader("If-None-Match")->value() == k.etag) {
        r = req->beginResponse(304, contentType, (const uint8_t*)"", 0);   // sent at once, unlike send(304)
    } else {
        r = req->beginResponse(LittleFS, k.gz, contentType);
        r->addHeader("Content-Encoding", "gzip");
    }
    r->addHeader("ETag", k.etag);
    r->addHeader("Cache-Control", "no-cache");
    req->send(r);
}

static void refreshFeBuildTime()
{
    String page = webFilePath("/index.html");
    if (page.length())
    {
        File f = LittleFS.open(page, "r");
        if (f) {
            time_t t = f.getLastWrite();
            f.close();
            if (t > 0) {
                struct tm ltm;
                gmtime_r(&t, &ltm);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
                g_feBuildTime = String(buf);
            }
        }
    } else {
        CLOCK_LOG("WebUI", "ERROR: /index.html is MISSING from the file system!");
    }
}

// Counts JSON API responses sent (not a raw TCP/socket count - lwIP's actual
// connection list is in a private header not meant for application code).
// This is a simple, safe proxy for API request volume: divide by uptime to
// see the actual rate the device is being polled at, to sanity-check client-
// side polling intervals against what the device is really seeing.
static uint32_t g_jsonResponsesSent = 0;

void WebUI::sendJson(AsyncWebServerRequest *req, const JsonDocument &doc)
{
    String out;
    out.reserve(2048);
    serializeJson(doc, out);
    // Explicit charset - without it, some HTTP clients (confirmed:
    // PowerShell 5.1's Invoke-RestMethod, used by Data/tools/Import-Events.ps1)
    // fall back to a non-UTF-8 default when decoding the response body,
    // silently corrupting any Hebrew text in it before it ever reaches the
    // JSON parser - found via a real event ending up with jMonth="Unknown"
    // after round-tripping through /api/calendar/convert.
    // This send() overload builds an AsyncBasicResponse, which unconditionally
    // adds "Connection: close" (library default, WebResponses.cpp) - every
    // JSON response here forces the client to open a fresh TCP connection
    // for its next request instead of reusing this one. Not fixed now: only
    // matters when something fires many requests back-to-back (the one
    // confirmed case, fetchHebrewOnlyAges()'s N+1 pattern, is gone as of
    // this comment - see /api/events/list). Revisit if a future feature
    // reintroduces a request burst and connection reuse would meaningfully
    // help.
    req->send(200, "application/json; charset=utf-8", out);
    g_jsonResponsesSent++;
}

uint32_t WebUI::getJsonResponsesSent() { return g_jsonResponsesSent; }

// Normalizes a client-provided event type to the exact lowercase value the
// Settings page's dropdown uses (see Data/index.html's #ev-type) - a type
// that drifted in from an older file, a hand-edited import, or a direct API
// call bypassing the UI's closed dropdown would otherwise silently desync
// from what that <select> can actually show (it'd save fine, then display
// as blank every time that event is opened to edit again). Anything
// unrecognized becomes "", which every reader of this field already
// defaults to "birthday" for - same behavior as a type that was never set.
static String normalizeEventType(const String& raw) {
    String t = raw;
    t.trim();
    t.toLowerCase();
    if (t == "birthday" || t == "wedding" || t == "yahrzeit" || t == "other") return t;
    return "";
}

// Mirrors the Settings UI's own type->icon mapping (getIconForEventType()
// in index.html) - needed server-side too, since an Excel import never
// carries an icon at all (deliberately no icon column there: Excel can't
// preview the UI's actual icon set, so asking the user to choose one
// blind just pushes a decision they're not equipped to make - see
// CLAUDE.md). Whatever default lands here is still visible and changeable
// afterward in the UI, same as any manually-created event.
static const char* defaultIconForType(const char* type) {
    if (!strcmp(type, "birthday")) return "🎂";
    if (!strcmp(type, "wedding"))  return "💍";
    if (!strcmp(type, "yahrzeit")) return "🕯️";
    return "🗓️";
}

// True for "" or whitespace-only - an event with no real name shows up on
// the clock as a reminder for nobody.
static bool isBlankName(const char* s)
{
    for (; s && *s; s++) {
        if (!isspace((unsigned char)*s)) return false;
    }
    return true;
}

// Shared by the single-event upsert and bulk-import handlers below, so the
// JSON field mapping only exists once. Does not touch `id` - callers decide
// that separately, since the two differ on where an id comes from.
static PersonalEvent parseEventFields(JsonObject in)
{
    PersonalEvent ev{};
    strncpy(ev.name, in["name"] | "", sizeof(ev.name) - 1);
    ev.hasGregorian = in["hasG"] | false;
    ev.gDay         = in["gD"] | 0;
    ev.gMonth       = in["gM"] | 0;
    ev.gYear        = in["gY"] | 0;
    ev.afterSunset  = in["afterSunset"] | false;
    ev.hasJewish    = in["hasJ"] | false;
    ev.jDay         = in["jD"] | 0;

    // Robust Adar handling: "אדר א׳" → 6, "אדר ב׳" → 13, "אדר" → 6 (Standard)
    int m = JewishDateEngine::getMonthFromName(in["jMonth"] | "תשרי");
    bool adar2 = in["isAdarII"] | false;
    ev.jMonth = (m == 6 && adar2) ? 13 : m;

    ev.jYear = in["jYNum"].is<int>() ? in["jYNum"].as<int>() : (in["jY"] | 0);
    strncpy(ev.type,         normalizeEventType(in["type"] | "birthday").c_str(), sizeof(ev.type) - 1);
    strncpy(ev.icon,         in["icon"]         | "",         sizeof(ev.icon) - 1);
    if (!ev.icon[0]) strncpy(ev.icon, defaultIconForType(ev.type), sizeof(ev.icon) - 1);
    const char* image = in["image"] | "";   // a picture from the library (POST /api/images), or none
    if (ImageLibrary::isValidId(image)) strlcpy(ev.image, image, sizeof(ev.image));
    ev.warningDays = in["warn"] | 0;
    return ev;
}

// If both calendars are flagged on but one side's actual date fields were
// never given, computes them from the other side - which must carry a
// year, since without one there's no single day to compute (a Gregorian
// month/day's Hebrew equivalent drifts every year; there's no "perpetual"
// Both). Runs BEFORE validateHebrewGregorianCombination() so there's
// something concrete for it to check; a "Both" row that's genuinely
// hand-filled on both sides (however inconsistently) is left untouched for
// that function to validate as usual. Shared by the single-event upsert
// and bulk-import handlers - this is also what the settings page's Excel upload's
// "Both, only one side filled in" rows rely on, rather than computing the
// missing side itself over HTTP.
//
// "Empty" is judged from day alone, not month: the Web UI's month fields
// are <select> dropdowns, which always have *some* option selected even
// when that calendar was never checked/intended (confirmed live - the
// Hebrew month shows a leftover value while its checkbox sits unchecked),
// so a zero month proves nothing. The day field is a plain input that the
// UI reliably clears to empty whenever that side isn't in use (both for a
// new event and when editing one with that side unchecked) - that's the
// only field worth trusting as a real "nothing here" signal.
static void autoFillMissingCalendarSide(PersonalEvent& ev)
{
    if (!ev.hasGregorian || !ev.hasJewish) return;

    bool gEmpty = (ev.gDay == 0);
    bool hEmpty = (ev.jDay == 0);

    if (gEmpty && !hEmpty && ev.jYear > 0) {
        // Gregorian side missing; Hebrew side given, with a year - compute Gregorian from it.
        HebrewDate h = { ev.jYear, ev.jMonth, ev.jDay };
        GregorianDate g = JewishDateEngine::calculateGregorian(h);
        ev.gYear = g.year; ev.gMonth = g.month; ev.gDay = g.day;
    } else if (hEmpty && !gEmpty && ev.gYear > 0) {
        // Hebrew side missing; Gregorian side given, with a year - compute Hebrew from it.
        GregorianDate g = { ev.gYear, ev.gMonth, ev.gDay, ev.afterSunset };
        HebrewDate h = JewishDateEngine::calculateHebrew(g);
        ev.jYear = h.year; ev.jMonth = h.month; ev.jDay = h.day;
    }
}

static bool validateHebrewGregorianCombination(PersonalEvent &ev, String &err)
{
    if (!ev.hasGregorian || !ev.hasJewish) return true;

    GregorianDate g = { ev.gYear, ev.gMonth, ev.gDay, ev.afterSunset };
    HebrewDate h    = { ev.jYear, ev.jMonth, ev.jDay };

    if (!JewishDateEngine::isValidCombination(g, h, err)) {
        // Legacy Recovery: Check if toggling 'afterSunset' fixes the mismatch.
        GregorianDate altG = g;
        altG.isAfterSunset = !g.isAfterSunset;
        String dummyErr;
        if (JewishDateEngine::isValidCombination(altG, h, dummyErr)) {
            ev.afterSunset = altG.isAfterSunset;
            CLOCK_LOG("WebUI", "Recovered event '%s' by toggling afterSunset to %s",
                      ev.name, ev.afterSunset ? "TRUE" : "FALSE");
            return true;
        }

        HebrewDate expected = JewishDateEngine::calculateHebrew(g);
        char hExpectedBuf[64], hProvidedBuf[64];
        JewishDateEngine::formatHebrewDate(expected, hExpectedBuf, sizeof(hExpectedBuf));
        JewishDateEngine::formatHebrewDate(h, hProvidedBuf, sizeof(hProvidedBuf));

        String gStr = String(ev.gYear) + "-" + String(ev.gMonth) + "-" + String(ev.gDay);
        if (ev.afterSunset) gStr += " (after sunset)";

        err = "Date mismatch. Gregorian [" + gStr + "] converts to Hebrew [" +
              String(hExpectedBuf) + "], but record provided [" + String(hProvidedBuf) + "]";
        return false;
    }
    return true;
}

static bool getBody(AsyncWebServerRequest *req, String &out)
{
    if (req->hasParam("body", true))
    {
        out = req->getParam("body", true)->value();
        return true;
    }
    return false;
}

// ─── Event pictures (ImageLibrary.h) ─────────────────────────────────────────

static const char* IMAGE_UPLOAD_TMP = "/upload_image.tmp";

// What a picture file is, if the screen can show it: a 16-bit BMP whose
// frames (biXPelsPerMeter, as Display.cpp reads it) fit the screen.
struct ImageInfo { int width = 0, height = 0, frames = 1, frameMs = 0; size_t bytes = 0; };
static bool readImageInfo(const char* path, ImageInfo& info) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    uint8_t h[46];
    bool ok = f.read(h, sizeof(h)) == sizeof(h) && h[0] == 'B' && h[1] == 'M';
    info.bytes = f.size();
    f.close();
    if (!ok) return false;
    int32_t w, ht, xPels, yPels; uint16_t bpp;
    memcpy(&w, h + 18, 4); memcpy(&ht, h + 22, 4); memcpy(&bpp, h + 28, 2);
    memcpy(&xPels, h + 38, 4); memcpy(&yPels, h + 42, 4);
    int frames = bmpFrameCount(w, ht, xPels);
    info.width = w / frames; info.height = ht; info.frames = frames;
    info.frameMs = (frames > 1 && yPels >= 20 && yPels <= 2000) ? yPels : 0;
    return bpp == 16 && w > 0 && info.width <= TFT_WIDTH && ht > 0 && ht <= TFT_HEIGHT &&
           info.bytes >= (size_t)w * ht * 2;
}

// ─── Replacements for the clock's own pictures (FS_CUSTOM_DIR, Config.h) ─────

static const char* CUSTOM_UPLOAD_TMP = "/upload_custom.tmp";

// A name of one of the clock's own pictures ("Candles" for /IMG/Candles.bmp).
static bool isBuiltinName(const String& name) {
    if (!name.length() || name.length() > 32) return false;
    for (size_t i = 0; i < name.length(); i++) if (!isalnum((unsigned char)name[i])) return false;
    String path = String("/IMG/") + name + ".bmp";
    DisplayManager::ImageFile inMemory;   // asked first: no flash read (see imageResponse())
    return DisplayManager::imageFile(path.c_str(), inMemory) || LittleFS.exists(path);
}

// A picture file for download: from the screen's memory when it has it
// (DisplayManager::imageFile() - the same bytes, but no flash read, which
// would pause the screen), else from flash. nullptr if there's no such file.
// ?fromFile=1 forces flash - only to compare the two (displayHitches in
// /api/status), nothing in the page uses it.
static AsyncWebServerResponse* imageResponse(AsyncWebServerRequest* req, const String& path) {
    auto file = std::make_shared<DisplayManager::ImageFile>();
    if (!req->hasParam("fromFile") && DisplayManager::imageFile(path.c_str(), *file)) {
        return req->beginResponse("image/bmp", file->size,
            [file](uint8_t* buf, size_t maxLen, size_t index) -> size_t { return file->read(buf, maxLen, index); });
    }
    if (!LittleFS.exists(path)) return nullptr;
    return req->beginResponse(LittleFS, path, "image/bmp");
}

// Bytes all replacements take now, leaving out `except` (the one being replaced).
static size_t customImagesBytes(const char* except) {
    size_t sum = 0;
    File dir = LittleFS.open(FS_CUSTOM_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String n = f.name();
        if (!except || n != String(except) + ".bmp") sum += f.size();
        f.close();
    }
    dir.close();
    return sum;
}

// Pictures no event uses any more (e.g. after deleting the last event that
// showed it). Not after an Excel import: its Undo puts back events whose
// pictures must still be there - the next restart cleans up instead.
static void removeUnusedImages() {
    EventsLock::take();
    ImageLibrary::removeUnused(*g_events);
    EventsLock::give();
}

// ESPAsyncWebServer's own ON_STA_FILTER (WebServer.cpp) checks that THIS
// request's destination IP exactly equals WiFi.localIP() (the STA IP) -
// true only for traffic arriving on the STA interface, false for anything
// arriving via the synchronized AP (WiFiManager::startAP(), 192.168.4.1),
// even once STA is connected. Every route below was filtered with that
// before the synchronized AP existed - its whole purpose is reaching the
// full app without the home network, so AP-arriving traffic needs the same
// access STA traffic gets, not none at all. Without this fix "/" itself
// rejects AP traffic, onNotFound redirects right back to "/", and the
// browser sees an infinite redirect loop (confirmed live 2026-10-15).
// Checking WiFi.status() instead of the specific interface preserves the
// original behavior during pure AP-only setup mode (WiFiManager::
// startAPPortal(), before Wi-Fi has ever been configured) - STA is never
// WL_CONNECTED there either way, so these routes still return nothing
// until setup completes, exactly as before.
static bool ON_STA_OR_SYNCED_AP_FILTER(AsyncWebServerRequest *req) {
    return WiFi.status() == WL_CONNECTED;
}

// The few routes a restore from the setup page needs (factory-new clock, no
// Wi-Fi known yet: only its setup access point, which has a password - the
// same trust as setting the Wi-Fi there). Everything else stays closed in
// setup mode, as above.
static bool ON_ANY_INCL_SETUP_FILTER(AsyncWebServerRequest *req) { return true; }

// Factory reset asked for (POST /api/factory-reset): done by loop() on the
// main task, after the reply has gone out.
static std::atomic<bool> g_factoryResetRequested{false};

// ─── begin() ─────────────────────────────────────────────────────────────────

void WebUI::begin(AppConfig &cfg, EventList &events)
{
    g_cfg = &cfg;
    g_events = &events;

    static bool initialized = false;
    if (initialized) return;

    refreshFeBuildTime();

    g_server = getAsyncServer();

    // ── Static files ──────────────────────────────────────────────────────────

    g_server->on("/", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        CLOCK_LOG("WebUI", "Debug: Request GET / from IP: %s", req->client()->remoteIP().toString().c_str());

        if (!NTPManager::isSynced() && Calendar::getToday().hYear == 0) {
            String loadingHtml = "<html><head><meta http-equiv='refresh' content='3'></head><body style='background:#000;color:#fff;text-align:center;padding-top:100px;font-family:sans-serif;'>";
            loadingHtml += "<h1>" + StringTools::formatProjectName(g_cfg->projectName) + " is initializing...</h1><p>Synchronizing time and calendar data. Please wait.</p></body></html>";
            req->send(200, "text/html", loadingHtml);
            return;
        }

        // No template callback: PROJECT_NAME/BE_BUILD/FE_BUILD used to be
        // substituted here on every request via a %TOKEN% scan - removed in
        // favor of the client reading them from /api/status (projectNameText,
        // beBuild, feBuild), which it already polls. Lets index.html serve as
        // a genuinely static file again - no more %%-escaping CSS/JS percent
        // signs to survive the scanner (see CLAUDE.md, now removed there too).
        sendWebFile(req, "/index.html", "text/html");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    g_server->on("/update.html", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
                     if (webFilePath("/update.html").length()) sendWebFile(req, "/update.html", "text/html");
                     else req->send(200, "text/html", Updater::EMERGENCY_UPDATE_PAGE);
                 }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    g_server->on("/sortable.min.js", HTTP_GET, [](AsyncWebServerRequest *req)
                 { sendWebFile(req, "/sortable.min.js", "application/javascript"); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // Third-party licenses (utils/make_licenses.py) - streamed from the file, ~100 KB.
    g_server->on("/licenses.txt", HTTP_GET, [](AsyncWebServerRequest *req)
                 { sendWebFile(req, "/licenses.txt", "text/plain; charset=utf-8"); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // The start guide (English + Hebrew, printable) and the full manual - in
    // every update package, so they match the version (2026-11-04).
    g_server->on("/start.html", HTTP_GET, [](AsyncWebServerRequest *req)
                 { sendWebFile(req, "/start.html", "text/html"); }).setFilter(ON_ANY_INCL_SETUP_FILTER);
    g_server->on("/manual.html", HTTP_GET, [](AsyncWebServerRequest *req)
                 { sendWebFile(req, "/manual.html", "text/html"); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // The page in restore-only view (it checks its own address): "Restore
    // from backup" on the setup page links here - a factory-new clock, put
    // back in one step from an Excel backup (settings incl. Wi-Fi, events,
    // pictures). Reachable in setup mode.
    g_server->on("/restore", HTTP_GET, [](AsyncWebServerRequest *req)
                 { sendWebFile(req, "/index.html", "text/html"); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // ── /api/factory-reset (POST) ─────────────────────────────────────────────
    // Everything of the clock's own goes (Storage::factoryReset()), then it
    // restarts into setup mode. Done by loop() after this reply.
    g_server->on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        DiagnosticLog::record("FACTORY", "factory reset requested from the page");
        g_factoryResetRequested = true;
        req->send(200, "text/plain", "Resetting - the clock restarts as new"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/status ───────────────────────────────────────────────────────────

    g_server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        JsonDocument doc;
        doc["firmware"]   = FIRMWARE_VERSION;
        doc["ssid"]       = WiFi.SSID();
        doc["rssi"]       = WiFi.RSSI();
        doc["ip"]         = WiFi.localIP().toString();
        doc["synced"]     = NTPManager::isSynced();
        doc["use24Hour"]  = g_cfg->use24Hour;
        doc["beBuild"]    = StringTools::getBuildTimestamp();
        doc["feBuild"]    = g_feBuildTime;
        // Also in /api/config, but that's only fetched once on page load -
        // this one is polled every second, so a rename (saveName() posts to
        // /api/config directly, without re-fetching it) still shows up in
        // the page's title/header within a second instead of needing a
        // manual reload.
        doc["projectNameText"] = StringTools::formatProjectName(g_cfg->projectName);
        // The clock's own Wi-Fi's name (WiFiManager: the AP is named projectName)
        // - the pages tell the user to choose it again after a restart.
        doc["apSsid"] = g_cfg->projectName;

        // Today's Hebrew date as the CLOCK sees it (sunset/tzeit rollover
        // applied) - the page's Hebrew-view "today" highlight uses this
        // instead of converting the browser's civil date, which only ever
        // switched at midnight and only once per page load.
        const DayInfo& hToday = Calendar::getToday();
        if (hToday.hYear > 0) {
            doc["todayHDay"]   = hToday.hDay;
            doc["todayHMonth"] = (hToday.hMonth == 13 || hToday.hMonth == 6) ? "אדר" : JewishDateEngine::getMonthName(hToday.hMonth);
        }

        time_t utcSec = NTPManager::getTime();
        doc["utcTime"]    = (int64_t)utcSec;
        // The offset the clock's time runs on right now (its time-zone rule,
        // summer time included) - not a stored number.
        long utcOffsetSec = utcSec ? (long)(NTPManager::getLocalEpoch() - utcSec) : 0;
        doc["utcOffsetSec"] = utcOffsetSec;
        doc["tzOffset"]   = StringTools::formatOffsetHours(utcOffsetSec);
        doc["tzRule"]     = g_cfg->tzPOSIX;

        doc["msSinceMdnsAlive"] = millis() - g_lastMdnsAliveAt;
        doc["msSinceTier2"] = millis() - g_lastTier2At;

        // ── Read-only diagnostic fields (networking-reliability investigation) ──
        doc["freeHeap"]        = ESP.getFreeHeap();
        doc["maxAllocHeap"]    = ESP.getMaxAllocHeap();
        doc["minFreeHeapEver"] = ESP.getMinFreeHeap();
        doc["uptimeMs"]        = millis();
        doc["wifiDisconnects"] = WiFiManager::getDisconnectCount();
        doc["wifiReconnects"]  = WiFiManager::getReconnectCount();
        doc["apRebuilds"]      = WiFiManager::getApRebuildCount();
        doc["lastGeoSyncMs"]       = GeolocationService::getLastSyncDurationMs();
        doc["lastReconnectChainMs"] = WiFiManager::getLastReconnectChainMs();
        doc["mdnsTier2FromServer"]   = g_diagMdnsTier2FromServer;
        doc["mdnsTier2FromWatchdog"] = g_diagMdnsTier2FromWatchdog;
        doc["wifiMode"] = (int)WiFi.getMode(); // 0=NULL 1=STA 2=AP 3=AP_STA
        doc["logLevel"] = Logging::levelName(Logging::getLevel());

        // Light sensor and backlight now - shown next to the brightness settings.
        doc["lightSensorAvailable"] = AmbientLight::isSensorAvailable();
        doc["lightSensorLux"]       = AmbientLight::readLux();
        doc["brightnessPct"]        = BrightnessManager::currentPct();
        doc["roomPct"]              = roundf(BrightnessManager::lightPct(AmbientLight::readLux()) * 10) / 10;
        doc["roomDarkPct"]          = roundf(g_cfg->roomDarkPct * 10) / 10;    // learning moves them
        doc["roomLightPct"]         = roundf(g_cfg->roomLightPct * 10) / 10;
        doc["brightnessDuty"]       = BrightnessManager::currentDuty();
        doc["displaySpiHz"]         = TFT_SPI_HZ;
        doc["displayFrozen"]        = DisplayManager::isFrozen();
        doc["demo"]                 = MessageAggregator::isDemoActive();
        doc["displayBlitUs"]        = DisplayManager::getLastBlitUs();
        doc["displayHitches"]       = DisplayManager::getHitches(false);
        doc["displayBigHitches"]    = DisplayManager::getHitches(true);
        doc["screenStill"]          = DisplayManager::goodMomentToWrite(millis());   // a flash write now would be unseen
        doc["tePeriodUs"]           = DisplayManager::getTePeriodUs();   // 0 = no TE: frames not timed
        doc["updateState"]          = Updater::stateName();   // "trial" | "installing" | "ok" - see Updater.h
        doc["jsonResponsesSent"]    = WebUI::getJsonResponsesSent();
        {
            DiagnosticLog::WriteStats ls = DiagnosticLog::writeStats();
            JsonObject lw = doc["logWrites"].to<JsonObject>();
            lw["count"] = ls.writes; lw["over50ms"] = ls.over50ms; lw["over100ms"] = ls.over100ms;
            lw["maxUs"] = ls.maxUs;
        }

        NTPManager::DateTime dt;
        if (NTPManager::getDateTime(dt)) {
            char dateBuf[32];
            snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", dt.year, dt.month, dt.day);
            doc["date"] = dateBuf;
        } else {
            doc["date"] = "";
        }

        doc["hebrewDate"] = Calendar::formatUserHebrewDate(Calendar::getTodaySummary());

        static unsigned long lastStatusLog = 0;
        if (millis() - lastStatusLog > STATUS_LOG_INTERVAL_MS) {
            const char* ssid   = doc["ssid"].as<const char*>();
            const char* offset = doc["tzOffset"].as<const char*>();
            const char* hdate  = doc["hebrewDate"].as<const char*>();
            CLOCK_LOG("WebUI", "Status: SSID=%s, Synced=%s, Offset=%s, HDate=%s",
                      ssid ? ssid : "?",
                      doc["synced"].as<bool>() ? "YES" : "NO",
                      offset ? offset : "0",
                      hdate ? hdate : "?");
            lastStatusLog = millis();
        }

        sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/calendar/convert (POST) ──────────────────────────────────────────

    g_server->on("/api/calendar/convert", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }
        const char* from = doc["from"] | "g";

        int y = doc["y"] | 0;
        int d = doc["d"] | 0;
        int m = 0;

        if (strcmp(from, "h") == 0) {
            m = JewishDateEngine::getMonthFromName(doc["month"]);
            if ((doc["isAdarII"] | false) || strcmp(doc["month"] | "", "אדר ב׳") == 0) m = 13;
        } else {
            m = doc["m"] | 0;
        }

        JsonDocument resp;
        if (strcmp(from, "g") == 0) {
            GregorianDate g = { y, m, d, doc["afterSunset"] | false };
            HebrewDate h    = JewishDateEngine::calculateHebrew(g);
            resp["y"] = h.year;
            if (h.month == 13 || h.month == 6) {
                resp["month"] = "אדר";
                resp["isAdarII"] = (h.month == 13);
            } else {
                resp["month"] = JewishDateEngine::getMonthName(h.month);
                resp["isAdarII"] = false;
            }
            resp["d"] = h.day;
        } else {
            HebrewDate h    = { y, m, d };
            GregorianDate g = JewishDateEngine::calculateGregorian(h);
            resp["y"] = g.year;
            resp["m"] = g.month;
            resp["d"] = g.day;
        }
        WebUI::sendJson(req, resp); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/config (GET) ─────────────────────────────────────────────────────

    g_server->on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        JsonDocument doc;

        doc["projectName"]     = g_cfg->projectName;
        doc["projectNameText"] = StringTools::formatProjectName(g_cfg->projectName);

        for (int i = 0; i < WIFI_NETWORKS; i++) {   // "wifiSSID", "wifiSSID2", ...
            doc[wifiConfigKey("wifiSSID", i)]     = wifiSsidAt(*g_cfg, i);
            doc[wifiConfigKey("wifiPassword", i)] = wifiPasswordAt(*g_cfg, i)[0] ? "********" : "";
        }

        // The active place (the connected network's), and each network's.
        doc["country"]      = g_cfg->locationCountry;
        doc["city"]         = g_cfg->locationCity;
        doc["rolloverMode"] = static_cast<int>(g_cfg->rolloverMode);
        doc["lat"]          = g_cfg->latitude;
        doc["lon"]          = g_cfg->longitude;
        doc["inIsrael"]     = g_cfg->inIsrael;
        doc["tzid"]         = g_cfg->tzid;
        doc["tzPOSIX"]      = g_cfg->tzPOSIX;
        doc["connectedSlot"] = WiFiManager::connectedSlot();
        JsonArray placesArr = doc["places"].to<JsonArray>();
        for (int i = 0; i < WIFI_NETWORKS; i++) {
            const Place& p = g_cfg->places[i];
            JsonObject o = placesArr.add<JsonObject>();
            o["set"] = p.set; o["confirmed"] = p.confirmed;
            o["city"] = p.city; o["country"] = p.country; o["lat"] = p.lat; o["lon"] = p.lon;
            o["tzid"] = p.tzid; o["ruleKnown"] = p.tzPOSIX[0] != '\0' || Location::isIsrael(p);
        }

        doc["use24Hour"]      = g_cfg->use24Hour;
        doc["ntpServerCount"] = g_cfg->ntpServerCount;
        JsonArray ntp = doc["ntpServers"].to<JsonArray>();
        for (int i = 0; i < g_cfg->ntpServerCount; i++)
            ntp.add(g_cfg->ntpServers[i]);

        doc["language"] = g_cfg->language;

        doc["debugTimeOffset"] = g_cfg->debugTimeOffset;
        doc["rotationMode"]    = static_cast<int>(g_cfg->rotationMode);
        doc["messageDwellSec"] = g_cfg->messageDwellSec;
        doc["separator"]       = g_cfg->messageSeparator;
        doc["scrollSpeed"]     = g_cfg->scrollSpeed;
        doc["fontCharGapPx"]   = g_cfg->fontCharGapPx;
        doc["fontWordGapPx"]   = g_cfg->fontWordGapPx;
        doc["fontWidthScalePct"] = g_cfg->fontWidthScalePct;

        doc["brightPct"]        = g_cfg->brightPct;
        doc["useLightSensor"]   = g_cfg->useLightSensor;
        doc["nightPct"]         = g_cfg->nightPct;
        doc["dayPct"]           = g_cfg->dayPct;
        doc["roomDarkPct"]      = roundf(g_cfg->roomDarkPct * 10) / 10;
        doc["roomLightPct"]     = roundf(g_cfg->roomLightPct * 10) / 10;
        doc["learnRoomLight"]   = g_cfg->learnRoomLight;

        doc["calibX"] = g_cfg->calibX;
        doc["calibY"] = g_cfg->calibY;
        doc["calibW"] = g_cfg->calibW;
        doc["calibH"] = g_cfg->calibH;
        doc["calibTearUs"] = g_cfg->calibTearUs;

        doc["showHebrewYear"]     = g_cfg->showHebrewYear;
        doc["showShabbatShalom"]  = g_cfg->showShabbatShalom;
        doc["showRoshChodesh"]    = g_cfg->showRoshChodesh;
        doc["showFasts"]          = g_cfg->showFasts;
        doc["omerMode"]           = static_cast<int>(g_cfg->omerMode);
        doc["showSpecialShabbat"] = g_cfg->showSpecialShabbat;
        doc["showLiturgical"]     = g_cfg->showLiturgical;
        doc["showSwitchover"]     = g_cfg->showSwitchoverPhrases;
        doc["prayerSwitchDays"]   = g_cfg->switchoverWindowDays;
        doc["nusach"]             = static_cast<int>(g_cfg->nusach);
        doc["eventDayChange"]     = static_cast<int>(g_cfg->eventDayChange);

        JsonArray orderArr = doc["contentOrder"].to<JsonArray>();
        for (int i = 0; i < (int)ContentType::COUNT; i++) orderArr.add(CONTENT_TYPE_KEYS[g_cfg->contentOrder[i]]);
        JsonObject freqObj = doc["contentFrequency"].to<JsonObject>();
        for (int t = 0; t < (int)ContentType::COUNT; t++) freqObj[CONTENT_TYPE_KEYS[t]] = g_cfg->contentFrequency[t];

        doc["pwdEnabled"] = g_cfg->webPasswordEnabled;
        doc["pwd"]        = (g_cfg->webPassword[0] != '\0') ? "********" : "";

        sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/config (POST) ────────────────────────────────────────────────────

    g_server->on("/api/config", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }

        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }

        if (doc["projectName"]) {
            String norm = StringTools::normalize(doc["projectName"] | "");
            strlcpy(g_cfg->projectName, norm.c_str(), sizeof(g_cfg->projectName));
        }

        // Known networks: a name ("" = remove that network) and/or a password
        // ("" or "********" = keep the saved one - unless the name changed:
        // then the old password belonged to another network and is dropped,
        // which also covers open networks). Used from the next connection on;
        // the current one is not dropped.
        for (int i = 0; i < WIFI_NETWORKS; i++) {
            const char* pwd = doc[wifiConfigKey("wifiPassword", i)];
            bool newPwd = pwd && pwd[0] && strcmp(pwd, "********") != 0;
            JsonVariant ssid = doc[wifiConfigKey("wifiSSID", i)];
            if (ssid.is<const char*>()) {
                const char* s = ssid.as<const char*>();
                if (strcmp(s, wifiSsidAt(*g_cfg, i)) != 0) {
                    if (!newPwd) wifiPasswordAt(*g_cfg, i)[0] = '\0';
                    Location::forgetPlace(i);   // another network: its place is unknown
                }
                strlcpy(wifiSsidAt(*g_cfg, i), s, sizeof(g_cfg->wifiSSID));
            }
            if (newPwd) strlcpy(wifiPasswordAt(*g_cfg, i), pwd, sizeof(g_cfg->wifiPassword));
        }

        // (The place isn't set here - it follows the network: /api/location/place.)
        if (doc["rolloverMode"].is<int>()) g_cfg->rolloverMode = static_cast<RolloverMode>(doc["rolloverMode"].as<int>());

        if (doc["use24Hour"].is<bool>()) g_cfg->use24Hour = doc["use24Hour"];
        JsonArray ntp = doc["ntpServers"].as<JsonArray>();
        if (ntp) {
            g_cfg->ntpServerCount = min((int)ntp.size(), 4);
            for (int i = 0; i < g_cfg->ntpServerCount; i++)
                strlcpy(g_cfg->ntpServers[i], ntp[i] | "", sizeof(g_cfg->ntpServers[i]));
        }

        if (doc["language"])              strlcpy(g_cfg->language, doc["language"] | "en", 4);
        if (doc["debugTimeOffset"].is<int64_t>()) {
            g_cfg->debugTimeOffset = doc["debugTimeOffset"];
            // Apply the offset immediately without a full re-init.
            NTPManager::setDebugOffset(g_cfg->debugTimeOffset);
        }
        if (doc["rotationMode"].is<int>()) g_cfg->rotationMode = static_cast<RotationMode>(doc["rotationMode"].as<int>());
        if (doc["messageDwellSec"].is<int>()) g_cfg->messageDwellSec = doc["messageDwellSec"];
        if (doc["separator"].is<const char*>()) strlcpy(g_cfg->messageSeparator, doc["separator"], 16);
        if (doc["scrollSpeed"].is<int>()) g_cfg->scrollSpeed = doc["scrollSpeed"];
        if (doc["fontCharGapPx"].is<int>()) g_cfg->fontCharGapPx = doc["fontCharGapPx"];
        if (doc["fontWordGapPx"].is<int>()) g_cfg->fontWordGapPx = doc["fontWordGapPx"];
        if (doc["fontWidthScalePct"].is<int>()) g_cfg->fontWidthScalePct = doc["fontWidthScalePct"];

        if (doc["brightPct"].is<int>()) g_cfg->brightPct = constrain((int)doc["brightPct"], 0, 100);
        if (doc["useLightSensor"].is<bool>()) g_cfg->useLightSensor = doc["useLightSensor"];
        // Night/Day: the clock shows the new level for a few seconds, so it
        // can be set by eye in the room itself.
        if (doc["nightPct"].is<float>()) { g_cfg->nightPct = constrain((float)doc["nightPct"], 0.0f, 100.0f); BrightnessManager::preview(g_cfg->nightPct); }
        if (doc["dayPct"].is<float>())   { g_cfg->dayPct   = constrain((float)doc["dayPct"],   0.0f, 100.0f); BrightnessManager::preview(g_cfg->dayPct); }
        // The room's edges: set by the user; learning goes on from there.
        if (doc["roomDarkPct"].is<float>())  { g_cfg->roomDarkPct  = constrain((float)doc["roomDarkPct"],  0.0f, 100.0f); BrightnessManager::edgesChangedByUser(); }
        if (doc["roomLightPct"].is<float>()) { g_cfg->roomLightPct = constrain((float)doc["roomLightPct"], 0.0f, 100.0f); BrightnessManager::edgesChangedByUser(); }
        if (doc["learnRoomLight"].is<bool>()) { g_cfg->learnRoomLight = doc["learnRoomLight"]; BrightnessManager::edgesChangedByUser(); }

        if (doc["calibX"].is<int>()) g_cfg->calibX = doc["calibX"];
        if (doc["calibY"].is<int>()) g_cfg->calibY = doc["calibY"];
        if (doc["calibW"].is<int>()) g_cfg->calibW = doc["calibW"];
        if (doc["calibH"].is<int>()) g_cfg->calibH = doc["calibH"];
        if (doc["calibTearUs"].is<int>()) g_cfg->calibTearUs = constrain((int)doc["calibTearUs"], 0, 20000);

        if (doc["showHebrewYear"].is<bool>())      g_cfg->showHebrewYear        = doc["showHebrewYear"];
        if (doc["showShabbatShalom"].is<bool>())  g_cfg->showShabbatShalom     = doc["showShabbatShalom"];
        if (doc["showRoshChodesh"].is<bool>())     g_cfg->showRoshChodesh       = doc["showRoshChodesh"];
        if (doc["showFasts"].is<bool>())           g_cfg->showFasts             = doc["showFasts"];
        if (doc["omerMode"].is<int>())             g_cfg->omerMode              = static_cast<OmerMode>(doc["omerMode"].as<int>());
        if (doc["showSpecialShabbat"].is<bool>())  g_cfg->showSpecialShabbat    = doc["showSpecialShabbat"];
        if (doc["showLiturgical"].is<bool>())      g_cfg->showLiturgical        = doc["showLiturgical"];
        if (doc["showSwitchover"].is<bool>())      g_cfg->showSwitchoverPhrases = doc["showSwitchover"];
        if (doc["prayerSwitchDays"].is<int>())     g_cfg->switchoverWindowDays  = constrain(doc["prayerSwitchDays"].as<int>(), 0, 2);
        if (doc["nusach"].is<int>())               g_cfg->nusach                = doc["nusach"].as<int>() == 1 ? Nusach::Sefard : Nusach::Ashkenaz;
        if (doc["eventDayChange"].is<int>())       g_cfg->eventDayChange        = doc["eventDayChange"].as<int>() == 1 ? EventDayChange::Midnight : EventDayChange::WithHebrewDay;

        if (doc["contentOrder"].is<JsonArray>()) {
            JsonArray orderArr = doc["contentOrder"];
            int i = 0;
            for (JsonVariant v : orderArr) {
                if (i >= (int)ContentType::COUNT) break;
                const char* key = v.as<const char*>();
                for (int t = 0; t < (int)ContentType::COUNT; t++) {
                    if (key && strcmp(key, CONTENT_TYPE_KEYS[t]) == 0) { g_cfg->contentOrder[i] = (uint8_t)t; break; }
                }
                i++;
            }
        }
        if (doc["contentFrequency"].is<JsonObject>()) {
            JsonObject freqObj = doc["contentFrequency"];
            for (int t = 0; t < (int)ContentType::COUNT; t++) {
                if (freqObj[CONTENT_TYPE_KEYS[t]].is<int>()) g_cfg->contentFrequency[t] = freqObj[CONTENT_TYPE_KEYS[t]].as<int>();
            }
        }

        if (doc["pwdEnabled"].is<bool>()) g_cfg->webPasswordEnabled = doc["pwdEnabled"];

        // Clear the last message cache in main.cpp to force a display refresh
        // with the new settings (e.g., switching rotation mode).

        // Written to flash after a short quiet period, not on every change -
        // see markConfigChanged(). Some settings (TZ, NTP server) still only
        // take full effect after a restart.
        markConfigChanged(doc.as<JsonObjectConst>());

        // Apply any display changes immediately
        DisplayManager::setWindow(g_cfg->calibX, g_cfg->calibY, g_cfg->calibW, g_cfg->calibH);
        DisplayManager::setTearUs(g_cfg->calibTearUs);

        req->send(200, "text/plain", "Saved"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/debug/time (POST) ────────────────────────────────────────────────

    g_server->on("/api/debug/time", HTTP_POST, [](AsyncWebServerRequest *req) {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }

        // The offset is "wanted time minus the clock's own time" - meaningless
        // before the first NTP sync (the clock still counts from 1970): taken
        // then, real time + offset later overflowed and the clock jumped to
        // 1948 (seen 2026-10-18, a request sent while it was still starting).
        if (!NTPManager::isSynced()) { req->send(409, "text/plain", "Clock time not synced yet - try again in a few seconds"); return; }
        // Three ways to say it (2026-10-21) - whichever comes replaces what
        // was set, as the clock keeps just one offset: "offset" = shift by
        // seconds (0 = real time), "local" = "YYYY-MM-DDTHH:MM" in the clock's
        // own time zone (the page's "Set to"), "timestamp" = UTC seconds
        // (scripts, sim/moments.py). Either way the clock runs on from there.
        int64_t realNow = (int64_t)time(nullptr), offset = 0;
        int Y, M, D, h, mi;
        if (doc["offset"].is<int64_t>()) {
            offset = doc["offset"].as<int64_t>();
        } else if (doc["local"].is<const char*>() && sscanf(doc["local"].as<const char*>(), "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &mi) == 5) {
            struct tm t = {};
            t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
            offset = (int64_t)mktime(&t) - realNow;   // mktime: the clock's time zone (TZ, set by NTPManager)
        } else if (doc["timestamp"].is<int64_t>()) {
            offset = doc["timestamp"].as<int64_t>() - realNow;
        } else {
            req->send(400, "text/plain", "Expected offset, local or timestamp");
            return;
        }
        g_cfg->debugTimeOffset = offset;
        NTPManager::setDebugOffset(offset);
        Storage::saveConfig(*g_cfg);
        // The calendar for the new date right away if it isn't saved - no
        // restart, no waiting out the 5-minute spacing between downloads.
        Calendar::timeJumped();
        DiagnosticLog::record("Debug", offset ? "time shifted by %lld s" : "back to real time (%lld)", (long long)offset);
        NTPManager::DateTime dt;
        char now[24] = "";
        if (NTPManager::getDateTime(dt)) snprintf(now, sizeof(now), "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
        req->send(200, "application/json", String("{\"offset\":") + String((long long)offset) + ",\"now\":\"" + now + "\"}");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/debug/screenshot (GET) ───────────────────────────────────────────
    // What the screen shows right now, as a 24-bit BMP (480x128, ~180 KB):
    // lets the PC simulator be checked against the real clock pixel for
    // pixel, and layout be checked without anyone looking at the clock. What
    // the frame is showing (as text): /api/preview. The frame is copied once
    // into a fixed PSRAM buffer and streamed from there in small chunks -
    // never built as one big response in RAM (see the DiagnosticLog gotcha
    // in CLAUDE.md). One screenshot at a time; a second overlapping request
    // simply gets the newer frame.
    g_server->on("/api/debug/screenshot", HTTP_GET, [](AsyncWebServerRequest *req) {
        static uint16_t* frame = nullptr;
        if (!frame) frame = (uint16_t*)heap_caps_malloc((size_t)TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        std::vector<OnScreenElement> shown;
        int scrollX = 0;
        if (!frame || !DisplayManager::copyFrame(frame, &shown, &scrollX)) { req->send(503, "text/plain", "No frame available"); return; }
        static const size_t HEADER = 54, ROW = TFT_WIDTH * 3, TOTAL = HEADER + ROW * TFT_HEIGHT;
        AsyncWebServerResponse *res = req->beginResponse("image/bmp", TOTAL, [](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
            size_t n = 0;
            for (; n < maxLen && index + n < TOTAL; n++) {
                size_t pos = index + n;
                if (pos < HEADER) {
                    // BITMAPFILEHEADER + BITMAPINFOHEADER, 24 bpp, bottom-up rows.
                    uint8_t h[HEADER] = {'B', 'M'};
                    auto put32 = [&h](int at, uint32_t v) { for (int k = 0; k < 4; k++) h[at + k] = (uint8_t)(v >> (8 * k)); };
                    put32(2, TOTAL); put32(10, HEADER); put32(14, 40);
                    put32(18, TFT_WIDTH); put32(22, TFT_HEIGHT);
                    h[26] = 1; h[28] = 24; put32(34, ROW * TFT_HEIGHT);
                    buf[n] = h[pos];
                    continue;
                }
                size_t p = pos - HEADER;
                int row = TFT_HEIGHT - 1 - (int)(p / ROW), col = (int)((p % ROW) / 3), ch = (int)((p % ROW) % 3);
                uint16_t c = frame[row * TFT_WIDTH + col];   // RGB565
                buf[n] = ch == 0 ? (uint8_t)((c & 0x1F) << 3)            // blue
                       : ch == 1 ? (uint8_t)(((c >> 5) & 0x3F) << 2)     // green
                                 : (uint8_t)(((c >> 11) & 0x1F) << 3);   // red
            }
            return n;
        });
        // What this frame shows, from the same instant (headers are ASCII, so
        // the text is percent-encoded UTF-8): elements joined by " | ",
        // images as [path].
        String text;
        for (size_t i = 0; i < shown.size(); i++) {
            if (i) text += " | ";
            text += shown[i].type == OnScreenElementType::TEXT ? shown[i].content : "[" + shown[i].content + "]";
        }
        String enc;
        static const char* HEX_DIGITS = "0123456789ABCDEF";
        for (size_t i = 0; i < text.length(); i++) {
            uint8_t ch = (uint8_t)text[i];
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') enc += (char)ch;
            else { enc += '%'; enc += HEX_DIGITS[ch >> 4]; enc += HEX_DIGITS[ch & 15]; }
        }
        res->addHeader("X-Screen-Text", enc);
        res->addHeader("X-Scroll-X", String(scrollX));
        req->send(res);
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/debug/diagnostic-log (GET / POST .../note) ──────────────────────

    g_server->on("/api/debug/diagnostic-log", HTTP_GET, [](AsyncWebServerRequest *req) {
        DiagnosticLog::sendTo(req);
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // Every flash operation since boot, counted and timed (FlashStats.h).
    g_server->on("/api/debug/flash", HTTP_GET, [](AsyncWebServerRequest *req) {
        static const char* NAMES[FlashStats::OPS] = { "read", "write", "erase" };
        FlashStats::Totals t = FlashStats::totals();
        JsonDocument doc;
        for (int i = 0; i < FlashStats::OPS; i++) {
            JsonObject o = doc[NAMES[i]].to<JsonObject>();
            o["count"] = t.count[i]; o["bytes"] = t.bytes[i]; o["us"] = t.us[i]; o["maxUs"] = t.maxUs[i];
        }
        FlashStats::SlowOp slow[32];
        int n = FlashStats::slowOps(slow, 32);
        JsonArray arr = doc["slow"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["atMs"] = slow[i].atMs; o["op"] = NAMES[slow[i].op]; o["us"] = slow[i].us;
            o["bytes"] = slow[i].bytes; o["task"] = slow[i].task;
        }
        doc["uptimeMs"] = millis();
        sendJson(req, doc);
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    g_server->on("/api/debug/diagnostic-log/note", HTTP_POST, [](AsyncWebServerRequest *req) {
        String msg = req->hasParam("msg", true) ? req->getParam("msg", true)->value()
                   : req->hasParam("msg")        ? req->getParam("msg")->value()
                   : "";
        if (msg.isEmpty()) { req->send(400, "text/plain", "Missing msg"); return; }
        DiagnosticLog::record("NOTE", "%s", msg.c_str());
        req->send(200, "text/plain", "Noted");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/location/detect (GET) ────────────────────────────────────────────

    // ── /api/location/... - each Wi-Fi network's place (Location.h) ──────────
    // Nothing on the bare "/api/location" (see the routing gotcha in CLAUDE.md).
    // All of them only queue work for the main loop - none blocks or changes
    // the configuration from this task.

    // detect (GET): one lookup (ip-api) for the network the clock is on.
    g_server->on("/api/location/detect", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        int slot = WiFiManager::connectedSlot();
        if (slot >= 0) Calendar::requestLookup(slot);
        JsonDocument resp;
        resp["started"] = slot >= 0;
        resp["slot"] = slot;
        WebUI::sendJson(req, resp);
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // place (POST {slot, city, country, lat, lon, tzid, offsetSec}): the place
    // chosen in the page for network `slot` (1-based in the page, 0-based here).
    g_server->on("/api/location/place", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }
        Place p = {};
        strlcpy(p.city,    doc["city"]    | "", sizeof(p.city));
        strlcpy(p.country, doc["country"] | "", sizeof(p.country));
        strlcpy(p.tzid,    doc["tzid"]    | "", sizeof(p.tzid));
        p.lat = doc["lat"] | 999.0f;
        p.lon = doc["lon"] | 999.0f;
        p.offsetSec = doc["offsetSec"] | 0;
        String error;
        if (!Location::queuePlace(doc["slot"] | -1, p, error)) { req->send(400, "text/plain", error); return; }
        req->send(200, "text/plain", "Saved"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // search (POST {q}) / search-result (GET): a place by name, looked up by
    // the clock (the browser may have no internet - the clock's own Wi-Fi).
    g_server->on("/api/location/search", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }
        String q = doc["q"] | "";
        q.trim();
        if (q.length() < 2 || q.length() > 60) { req->send(400, "text/plain", "Type at least 2 letters"); return; }
        Location::requestSearch(q);
        req->send(200, "text/plain", "Searching"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    g_server->on("/api/location/search-result", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        req->send(200, "application/json", Location::searchStatus()); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/calibration/start & /stop ────────────────────────────────────────
    //
    // Deliberately no handler on the bare "/api/calibration" path - see the
    // ESPAsyncWebServer routing gotcha in CLAUDE.md. There used to be one
    // here (saving calibX/Y/W/H from "x"/"y"/"w"/"h" keys), but /api/config
    // already does the exact same job (same fields, same DisplayManager::
    // setWindow() call - see saveAdvanced() in index.html, which is the only
    // thing that ever actually sent calibration coordinates). With nothing
    // live ever POSTing to the bare path with its expected keys, its only
    // real effect was silently swallowing every request to the two routes
    // below, since "/api/calibration" prefix-matches "/api/calibration/start"
    // and "/api/calibration/stop" - removed rather than renamed, since it
    // had no caller left to preserve.

    // start (POST {pattern: bool}): the window frame on the live content
    // (false) or on the numbered test pattern (true) - see Display.cpp.
    g_server->on("/api/calibration/start", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        String body;
        JsonDocument doc;
        bool pattern = getBody(req, body) && !deserializeJson(doc, body) && (doc["pattern"] | false);
        DisplayManager::setCalibrationPattern(pattern);
        DisplayManager::setMode(DisplayMode::Calibration);
        req->send(200, "text/plain", pattern ? "Calibration: test pattern" : "Calibration: content"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    g_server->on("/api/calibration/stop", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        DisplayManager::setCalibrationPattern(false);
        DisplayManager::setMode(DisplayMode::Normal);
        req->send(200, "text/plain", "Calibration off"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/calendar (GET) ───────────────────────────────────────────────────
    // No UI client calls this anymore (2026-10-08 - was the status-popup's
    // Hebrew date/parasha/holiday fields, removed with the popup). Kept as a
    // standalone, curl-able diagnostic endpoint - may be useful again for
    // debugging or a future feature.

    g_server->on("/api/calendar", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        JsonDocument doc;
        const DayInfo& today = Calendar::getToday();
        doc["hebrewDate"]   = Calendar::formatUserHebrewDate(today.hebrewDateStr);
        doc["parasha"]      = today.parashaHebrew;
        doc["holiday"]      = today.holidayHebrew;
        doc["candleTime"]   = today.candleLightStr;
        doc["havdalahTime"] = today.havdalahStr;
        doc["omerHebrew"]   = today.omerHebrew;
        doc["dafYomi"]      = today.dafYomi;
        doc["mishnaYomit"]  = today.mishnaYomit;
        doc["sunset"]       = today.sunset;
        doc["tzeit"]        = today.tzeit;
        doc["isFast"]       = today.isFast;

        CLOCK_LOG("WebUI", "[DataTrace] /api/calendar: HDate=%s, Parasha=%s, Daf=%s, Mishna=%s",
                  today.hebrewDateStr,
                  today.parashaHebrew[0] ? today.parashaHebrew : "empty",
                  today.dafYomi[0]       ? today.dafYomi       : "empty",
                  today.mishnaYomit[0]   ? today.mishnaYomit   : "empty");
        WebUI::sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/calendar/refresh (POST) ──────────────────────────────────────────

    g_server->on("/api/calendar/refresh", HTTP_POST, [](AsyncWebServerRequest *req) {
        Calendar::forceRefresh();
        req->send(200, "text/plain", "Manifest refresh triggered");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/calendar/clear-cache (POST) ──────────────────────────────────────

    g_server->on("/api/calendar/clear-cache", HTTP_POST, [](AsyncWebServerRequest *req) {
        Calendar::clearCacheAndRefresh();
        req->send(200, "text/plain", "Cache cleared. Refresh will occur on the next cycle.");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/debug/display-link (POST) ────────────────────────────────────────
    // Live display test from Advanced settings: freeze. Not saved. Current
    // state is in /api/status (displayFrozen / displayBlitUs). (A live SPI
    // speed switch was here; it never took effect - removed 2026-10-26.)

    g_server->on("/api/debug/display-link", HTTP_POST, [](AsyncWebServerRequest *req) {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "text/plain", "Invalid JSON"); return; }
        if (doc["frozen"].is<bool>()) DisplayManager::setFrozen(doc["frozen"].as<bool>());
        req->send(200, "text/plain", "Applied (until restart)");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/demo (POST) ──────────────────────────────────────────────────────
    // Advanced settings' Demo button: {"on": true} plays one pass of sample
    // slides, {"on": false} stops it. Not saved. State: /api/status "demo".
    g_server->on("/api/demo", HTTP_POST, [](AsyncWebServerRequest *req) {
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }
        JsonDocument doc;
        if (deserializeJson(doc, body) || !doc["on"].is<bool>()) { req->send(400, "text/plain", "Expected {\"on\": true|false}"); return; }
        bool on = doc["on"];
        MessageAggregator::requestDemo(on);
        req->send(200, "text/plain", on ? "Demo started" : "Demo stopped");
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/debug/log-level (POST) ───────────────────────────────────────────
    // Single enumerated verbosity dial (see LogLevel.h) rather than the
    // earlier one-off Hebcal-only toggle this replaces - deliberately not
    // per-area flags, which would be more to remember and more to get
    // wrong than the benefit is worth for a project this size.

    g_server->on("/api/debug/log-level", HTTP_POST, [](AsyncWebServerRequest *req) {
        // The Settings UI's apiPost() always wraps its payload as a single
        // JSON "body" param (see getBody()/the other /api/* POST handlers
        // above) - NOT individual top-level form fields. A plain
        // req->getParam("level", true) would silently never match anything
        // posted from the browser (only from a raw curl -d "level=..."),
        // so this reads the same way every other JSON-bodied endpoint does.
        String body;
        String level;
        bool persist = false;
        if (getBody(req, body)) {
            JsonDocument doc;
            if (!deserializeJson(doc, body)) {
                level = doc["level"] | "";
                // doc["persist"] | "0" yields a const char* - comparing that
                // with == "1" compares pointer addresses, not string content,
                // and is never true. Wrap in String first for a real compare.
                persist = String(doc["persist"] | "0") == "1";
            }
        }
        Logging::setLevel(Logging::parseLevelName(level.c_str(), Logging::getLevel()));

        // "persist" is opt-in: without it this only changes the live RAM
        // value, same as always - self-heals back to the saved default on
        // any reboot, including an unplanned one mid-investigation. Only an
        // explicit persist=1 writes it into config.json, so it survives on
        // purpose, not by accident.
        if (persist) {
            strlcpy(g_cfg->logLevel, Logging::levelName(Logging::getLevel()), sizeof(g_cfg->logLevel));
            Storage::saveConfig(*g_cfg);
        }

        req->send(200, "text/plain", String("Log level: ") + Logging::levelName(Logging::getLevel()) + (persist ? " (persisted)" : ""));
    }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/preview (GET) ────────────────────────────────────────────────────
    // No UI client calls this anymore (2026-10-08 - was the status-popup's
    // "on display now" preview, removed with the popup). Kept as a
    // standalone, curl-able diagnostic endpoint - may be useful again for
    // debugging or a future feature.

    g_server->on("/api/preview", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        JsonDocument doc;
        const auto elements = DisplayManager::getCurrentMessageElements(); // now an independent copy, not a reference into shared state
        String fullMessage;
        for (size_t i = 0; i < elements.size(); ++i) {
            const auto& p = elements[i];
            if (p.type == OnScreenElementType::TEXT) {
                fullMessage += p.content;
            } else {
                fullMessage += "[" + p.content + "]";
            }
            if (i < elements.size() - 1) {
                fullMessage += " * "; // Use a separator for readability
            }
        }
        doc["displayString"] = fullMessage;
        WebUI::sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/events/excel-template (GET) ─────────────────────────────────────
    //
    // The blank events spreadsheet. The settings page's "Download events as
    // Excel" fetches it and writes the current events into it in the browser
    // (keeping its Instructions tab, dropdowns and formatting); "Upload Excel
    // file" reads an edited copy back, also in the browser, and posts the
    // result to /api/events/import. Replaced the PowerShell import/export
    // scripts (2026-10-17) - no scripts, works on any device with a browser.
    g_server->on("/api/events/excel-template", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        req->send(LittleFS, "/tools/JewishClock_Events_Template.xlsx",
                  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/events/list (GET) ────────────────────────────────────────────────
    //
    // Its own named sub-path rather than plain "/api/events", for the same
    // reason as "/api/events/upsert" below - see that comment, and the
    // ESPAsyncWebServer routing gotcha in CLAUDE.md, for why a bare
    // "/api/events" handler of any method is avoided here on purpose.
    g_server->on("/api/events/list", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        EventsLock::take();
        for (const auto& e : *g_events) {
            JsonObject ev = arr.add<JsonObject>();
            ev["id"]          = e.id;
            ev["name"]        = e.name;
            ev["hasG"]        = e.hasGregorian;
            ev["gD"]          = e.gDay;
            ev["gM"]          = e.gMonth;
            ev["gY"]          = e.gYear;
            ev["afterSunset"] = e.afterSunset;
            ev["hasJ"]        = e.hasJewish;
            ev["jD"]          = e.jDay;
            ev["jMonth"]      = (e.jMonth == 13) ? "אדר" : JewishDateEngine::getMonthName(e.jMonth);
            ev["isAdarII"]    = (e.jMonth == 13);

            int yVal = e.jYear;
            ev["jYNum"] = yVal;
            char yHeb[32];
            if (yVal >= 5000) JewishDateEngine::yearToGematria(yVal, yHeb, sizeof(yHeb));
            else              JewishDateEngine::numToGematria(yVal, yHeb, sizeof(yHeb));
            ev["jY"]           = yHeb;
            ev["icon"]         = e.icon;
            ev["warn"]         = e.warningDays;
            ev["type"]         = e.type[0]         ? e.type         : "birthday";
            ev["image"]        = e.image;   // picture library id, "" = none

            // Hebrew-only events need a Gregorian equivalent for the UI's
            // age display (computeAge() in index.html only knows Gregorian
            // math). Computed here, once, server-side, using the same
            // JewishDateEngine call /api/calendar/convert uses - NOT left
            // to the client to fetch per-event. That was the original
            // design (fetchHebrewOnlyAges() in index.html, one POST per
            // Hebrew-only event) and it measurably stalled page load: with
            // ~26 such events it fired ~26 separate requests, and since
            // every response here carries "Connection: close" (see
            // WebUI::sendJson()), each one paid a fresh TCP handshake,
            // piling up behind Chrome's 6-connections-per-origin limit -
            // a HAR capture showed onLoad at ~2.58s, almost all of it this.
            if (e.hasJewish && !e.hasGregorian && e.jYear > 0) {
                HebrewDate h = { e.jYear, e.jMonth, e.jDay };
                GregorianDate g = JewishDateEngine::calculateGregorian(h);
                ev["gAgeY"] = g.year;
                ev["gAgeM"] = g.month;
                ev["gAgeD"] = g.day;
            }
        }
        EventsLock::give();
        sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/events/delete and /api/events/import ─────────────────────────────
    //
    // Originally these were silently swallowed by a plain "/api/events" POST
    // handler registered earlier - ESPAsyncWebServer treats a registered URI
    // as matching that path OR any deeper sub-path, dispatching to whichever
    // matching handler was registered first, so "/api/events/delete" and
    // "/api/events/import" both got routed into the plain handler instead of
    // their own code (surfaced live as both returning its "Missing body"
    // error). Fixed for good by giving the single-event upsert handler its
    // own name instead - see "/api/events/upsert" below - so there's no
    // longer a plain "/api/events" POST handler for anything here to collide
    // with. Registration order among delete/import/upsert no longer matters;
    // kept in this relative position for readability, not correctness.

    g_server->on("/api/events/delete", HTTP_POST, [&](AsyncWebServerRequest *req)
                 {
        String id = req->hasParam("id", true) ? req->getParam("id", true)->value()
                  : req->hasParam("id")        ? req->getParam("id")->value()
                  : "";
        if (id.isEmpty()) { req->send(400, "text/plain", "Missing id"); return; }

        EventsLock::take();
        int idx = -1;
        for (size_t i = 0; i < g_events->size(); i++) {
            if (strcmp((*g_events)[i].id, id.c_str()) == 0) { idx = (int)i; break; }
        }
        if (idx < 0) {
            EventsLock::give();
            req->send(404, "text/plain", "Event not found");
            return;
        }

        g_events->erase(g_events->begin() + idx);
        bool saved = Storage::saveEvents(*g_events);
        EventsLock::give();
        if (!saved) {
            req->send(500, "text/plain", "Failed to save events");
            return;
        }
        removeUnusedImages();
        req->send(200, "text/plain", "Deleted"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/images/upload, /api/images/list, /api/images/get ────────────────
    // The picture library (ImageLibrary.h). Each under its own sub-path, none
    // on a bare "/api/images" (see the routing gotcha in CLAUDE.md).
    //
    // upload (POST, one file): the event form makes the BMP in the browser;
    // the clock checks it, names it by its CRC-32 and keeps one copy of each
    // picture - uploading one that is already there just returns its id.
    // Refused up front if it wouldn't fit while leaving room for updates.
    // Written to a temp file and renamed once checked. Replies {"id": "..."};
    // the event then refers to that id (upsert / import "image").
    static File     s_imgUpload;
    static uint32_t s_imgCrc = 0;
    static String   s_imgRefused;
    g_server->on("/api/images/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            if (s_imgUpload) s_imgUpload.close();
            ImageInfo info;
            String error = s_imgRefused;
            if (!error.length() && !readImageInfo(IMAGE_UPLOAD_TMP, info)) error = "Not a picture the clock can show";
            // One still picture only (2026-10-21): animations take too much of the clock's space.
            if (!error.length() && info.frames > 1) error = "Only a still picture - animations take too much room on the clock";
            String id = ImageLibrary::idFromCrc(s_imgCrc), path = imagePath(id.c_str());
            bool existed = false;
            if (!error.length()) {
                LittleFS.mkdir(FS_IMAGE_DIR);
                existed = LittleFS.exists(path);
                if (!existed && !LittleFS.rename(IMAGE_UPLOAD_TMP, path)) error = "Could not save the picture";
            }
            LittleFS.remove(IMAGE_UPLOAD_TMP);
            if (error.length()) { req->send(400, "text/plain", error); return; }
            DisplayManager::isImageUsable(path.c_str());   // into memory now: the list and downloads come from there
            DiagnosticLog::record("Images", "picture %s %s (%dx%d, %d frame(s), %u bytes)", id.c_str(),
                                  existed ? "already there" : "added", info.width, info.height, info.frames, (unsigned)info.bytes);
            req->send(200, "application/json", "{\"id\":\"" + id + "\",\"existed\":" + (existed ? "true" : "false") + "}");
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                if (s_imgUpload) s_imgUpload.close();
                s_imgCrc = 0;
                s_imgRefused = "";
                if (req->contentLength() > IMAGE_MAX_BYTES) s_imgRefused = "Picture file too big";
                else ImageLibrary::hasRoomFor(req->contentLength(), &s_imgRefused);
                if (!s_imgRefused.length()) s_imgUpload = LittleFS.open(IMAGE_UPLOAD_TMP, "w");
            }
            if (s_imgUpload) {
                if (s_imgUpload.write(data, len) != len) { s_imgRefused = "The clock could not store the picture"; s_imgUpload.close(); }
                s_imgCrc = ImageLibrary::crc32(s_imgCrc, data, len);
            }
            if (final && s_imgUpload) s_imgUpload.close();
        }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // list (GET): every picture, for the event form's "from the clock" choice.
    // From the screen's memory, not the files (see /api/builtin/list).
    g_server->on("/api/images/list", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        const String dirPrefix = FS_IMAGE_DIR "/";
        for (const auto& im : DisplayManager::imagesIn(dirPrefix.c_str())) {
            String id = im.path.substring(dirPrefix.length(), im.path.length() - 4);   // "/images/1a2b3c4d.bmp" -> "1a2b3c4d"
            if (!ImageLibrary::isValidId(id.c_str())) continue;
            JsonObject o = arr.add<JsonObject>();
            o["id"] = id; o["w"] = im.frameWidth; o["h"] = im.height;
            o["frames"] = im.frames; o["frameMs"] = im.frameMs; o["bytes"] = im.fileSize;
            int users = 0;
            EventsLock::take();
            for (const auto& e : *g_events) if (id == e.image) users++;
            EventsLock::give();
            o["usedBy"] = users;
        }
        sendJson(req, doc); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // get (GET ?id=): the picture file itself.
    g_server->on("/api/images/get", HTTP_GET, [](AsyncWebServerRequest *req) {
        String id = req->hasParam("id") ? req->getParam("id")->value() : "";
        AsyncWebServerResponse* res = ImageLibrary::isValidId(id.c_str()) ? imageResponse(req, imagePath(id.c_str())) : nullptr;
        if (!res) { req->send(404, "text/plain", "No such picture"); return; }
        res->addHeader("Cache-Control", "max-age=31536000, immutable");   // an id is a content checksum: never changes
        req->send(res); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);


    // ── /api/builtin/list, /get, /upload, /reset ──────────────────────────────
    // The clock's own pictures (/IMG: candles, Havdalah, parasha, Mishna, the
    // event icons) and the family's replacements for them (FS_CUSTOM_DIR,
    // Settings > Pictures, 2026-10-21). A replacement may be a short animation;
    // all of them together up to CUSTOM_IMAGES_MAX_BYTES. ?name= is the
    // picture's name ("Candles"), checked against /IMG.
    g_server->on("/api/builtin/list", HTTP_GET, [](AsyncWebServerRequest *req) {
        // From the screen's memory, which holds every one of these pictures
        // (DisplayManager::imagesIn()) - reading the 28 files' headers took
        // 0.9 s and froze the screen meanwhile (2026-10-22).
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        auto defaults = DisplayManager::imagesIn("/IMG/");
        auto customs  = DisplayManager::imagesIn(FS_CUSTOM_DIR "/");
        size_t used = 0;
        for (const auto& c : customs) used += c.fileSize;
        root["maxBytes"] = CUSTOM_IMAGES_MAX_BYTES;
        root["usedBytes"] = used;
        JsonArray arr = root["pictures"].to<JsonArray>();
        for (const auto& d : defaults) {
            String n = d.path.substring(5, d.path.length() - 4);   // "/IMG/Candles.bmp" -> "Candles"
            JsonObject o = arr.add<JsonObject>();
            o["name"] = n; o["w"] = d.frameWidth; o["h"] = d.height; o["frames"] = d.frames;
            for (const auto& c : customs) {
                if (c.path != customImagePath(n.c_str())) continue;
                o["custom"] = true; o["customFrames"] = c.frames; o["customBytes"] = c.fileSize;
            }
        }
        sendJson(req, doc); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // get (GET ?name=&which=default|custom): the picture file.
    g_server->on("/api/builtin/get", HTTP_GET, [](AsyncWebServerRequest *req) {
        String name = req->hasParam("name") ? req->getParam("name")->value() : "";
        bool custom = req->hasParam("which") && req->getParam("which")->value() == "custom";
        AsyncWebServerResponse* res = isBuiltinName(name)
            ? imageResponse(req, custom ? customImagePath(name.c_str()) : String("/IMG/") + name + ".bmp") : nullptr;
        if (!res) { req->send(404, "text/plain", "No such picture"); return; }
        req->send(res); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // upload (POST ?name=, one file): checked, then put in place of the
    // previous replacement (temp file + rename).
    static File   s_customUpload;
    static String s_customRefused, s_customName;
    g_server->on("/api/builtin/upload", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            if (s_customUpload) s_customUpload.close();
            ImageInfo info;
            // s_customName is set by the per-chunk handler and cleared below:
            // empty here = no file was sent with this request.
            String error = !s_customName.length() ? String("No picture was sent") : s_customRefused;
            if (!error.length() && !readImageInfo(CUSTOM_UPLOAD_TMP, info)) error = "Not a picture the clock can show";
            String path = customImagePath(s_customName.c_str());
            if (!error.length()) {
                LittleFS.mkdir(FS_CUSTOM_DIR);
                LittleFS.remove(path);
                if (!LittleFS.rename(CUSTOM_UPLOAD_TMP, path)) error = "Could not save the picture";
            }
            LittleFS.remove(CUSTOM_UPLOAD_TMP);
            String name = s_customName;
            s_customName = ""; s_customRefused = "";
            if (error.length()) { req->send(400, "text/plain", error); return; }
            DisplayManager::forgetImage(path.c_str());
            DisplayManager::isImageUsable(path.c_str());   // the new one into memory now: the lists come from there
            DiagnosticLog::record("Images", "%s replaced (%dx%d, %d frame(s), %u bytes)", name.c_str(),
                                  info.width, info.height, info.frames, (unsigned)info.bytes);
            req->send(200, "text/plain", "Picture replaced");
        },
        [](AsyncWebServerRequest *req, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                if (s_customUpload) s_customUpload.close();
                s_customRefused = "";
                s_customName = req->hasParam("name") ? req->getParam("name")->value() : "";
                size_t size = req->contentLength();
                size_t others = customImagesBytes(s_customName.c_str());
                if (!isBuiltinName(s_customName)) s_customRefused = "No such picture";
                else if (others + size > CUSTOM_IMAGES_MAX_BYTES)
                    s_customRefused = "Too big: replaced pictures may use " + String(CUSTOM_IMAGES_MAX_BYTES / 1024) + " KB together, " +
                                      String((CUSTOM_IMAGES_MAX_BYTES - others) / 1024) + " KB are left - a shorter animation fits";
                else ImageLibrary::hasRoomFor(size, &s_customRefused);
                if (!s_customRefused.length()) s_customUpload = LittleFS.open(CUSTOM_UPLOAD_TMP, "w");
            }
            if (s_customUpload && s_customUpload.write(data, len) != len) { s_customRefused = "The clock could not store the picture"; s_customUpload.close(); }
            if (final && s_customUpload) s_customUpload.close();
        }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // reset (POST ?name=): back to the clock's own picture.
    g_server->on("/api/builtin/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        String name = req->hasParam("name") ? req->getParam("name")->value() : "";
        if (!isBuiltinName(name)) { req->send(404, "text/plain", "No such picture"); return; }
        String path = customImagePath(name.c_str());
        LittleFS.remove(path);
        DisplayManager::forgetImage(path.c_str());
        DiagnosticLog::record("Images", "%s back to the clock's own picture", name.c_str());
        req->send(200, "text/plain", "Back to the clock's own picture"); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);
    // Backs the UI's "Import Events" button, kept separate from the
    // single-event upsert below because it's a different operation: that
    // button replaces the WHOLE list from a file (matching what "Export
    // Events" produces), not one record. Builds the full replacement list
    // before touching *g_events at all, for the same reason as upsert - a
    // bad file must never leave a half-replaced list.
    g_server->on("/api/events/import", HTTP_POST, [&](AsyncWebServerRequest *req)
                 {
        const AsyncWebParameter* p = req->getParam("body", true);
        if (!p) { req->send(400, "text/plain", "Missing body"); return; }

        JsonDocument doc;
        if (deserializeJson(doc, p->value()) || !doc.is<JsonArray>()) {
            req->send(400, "text/plain", "Invalid JSON array");
            return;
        }

        JsonArray arr = doc.as<JsonArray>();
        int count = min((int)arr.size(), MAX_EVENTS);

        // All-or-nothing like the rest of import: a nameless row rejects the
        // whole file, with every offending row listed, before anything changes.
        String blankRows;
        for (int i = 0; i < count; i++) {
            if (isBlankName(arr[i]["name"] | "")) {
                if (blankRows.length()) blankRows += ", ";
                blankRows += String(i + 1);
            }
        }
        if (blankRows.length()) {
            req->send(400, "text/plain", "Not imported: event(s) " + blankRows + " have no name. Nothing was changed.");
            return;
        }

        size_t freeSpiram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t totalSpiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        float freePct = totalSpiram ? (100.0f * freeSpiram / totalSpiram) : 100.0f;
        if (freePct < EVENTS_PSRAM_REFUSE_PCT) {
            req->send(400, "text/plain", "Not imported: only " + String(freePct, 1) + "% of PSRAM free (refusing below " + String(EVENTS_PSRAM_REFUSE_PCT, 0) + "%).");
            return;
        }

        EventList imported;
        JsonDocument respDoc;
        JsonArray warnings = respDoc["warnings"].to<JsonArray>();

        // The percentage-free check above is a point-in-time heuristic, not
        // a hard guarantee against PSRAM fragmentation - PsramAllocator
        // (PsramVector.h) still throws std::bad_alloc if growth genuinely
        // fails despite it passing. Caught here instead of crashing: on
        // failure the real event list (g_events, untouched until after this
        // whole block succeeds) is unaffected either way.
        try {
            imported.reserve(count);
            for (int i = 0; i < count; i++) {
                JsonObject in = arr[i];
                PersonalEvent ev = parseEventFields(in);
                autoFillMissingCalendarSide(ev);

                // Always mint a fresh id, ignoring any "id" in the input - this
                // is import's whole point: it's meant for hand-merged/handcrafted
                // files (export current, combine with other records in a text
                // editor, reimport), and trusting a hand-edited id risks two
                // distinct records sharing one by accident (e.g. a copy-pasted
                // event whose id never got changed) - import never treats two
                // records as "the same event" by id, only loadEvents() does.
                strncpy(ev.id, Storage::generateEventId(imported).c_str(), sizeof(ev.id) - 1);

                const char* name = ev.name[0] ? ev.name : "Unnamed event";
                String detail;
                if (!validateHebrewGregorianCombination(ev, detail)) {
                    JsonObject w = warnings.add<JsonObject>();
                    w["name"] = name;
                    w["detail"] = detail;
                }
                imported.push_back(ev);
            }
        } catch (const std::bad_alloc&) {
            req->send(400, "text/plain", "Not imported: ran out of PSRAM partway through - existing events unchanged.");
            return;
        }

        bool saved;
        int savedCount;
        // Pictures come along by id ("image"; the page uploads the file's
        // pictures first). Unused ones are NOT removed here - Undo may need
        // them; see removeUnusedImages().
        {
            EventsLock::Guard guard; // RAII: releases even if the line below throws.
            try {
                *g_events = imported;
            } catch (const std::bad_alloc&) {
                req->send(400, "text/plain", "Import failed: ran out of PSRAM copying into the live list. Please check the Events list and try again.");
                return;
            }
            saved = Storage::saveEvents(*g_events);
            savedCount = (int)g_events->size();
        }
        if (!saved) {
            req->send(500, "text/plain", "Failed to save events");
            return;
        }
        respDoc["saved"] = true;
        respDoc["count"] = savedCount;
        sendJson(req, respDoc); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // ── /api/events/upsert (POST) - upsert a single event ────────────────────
    //
    // Deliberately its own sub-path, not plain "/api/events" - with every
    // POST handler under "/api/events/" living on a distinct segment, no
    // handler's URI is ever a strict prefix of another's, which is what
    // actually caused the delete/import routing bug the registration-order
    // comments above describe. (Registering a plain "/api/events" POST
    // handler would still collide with "/api/events/delete" etc. regardless
    // of order - the fix here is giving upsert its own name, not just
    // ordering around the collision.)
    //
    // The client sends one event object (optionally carrying an "id" from a
    // prior GET, when editing). This builds and validates the candidate
    // record FIRST, entirely separately from the live vector - only once
    // it's known good does it either replace an existing entry in place (by
    // index, preserving display order) or append a new one. That ordering
    // means a bad request can never leave an existing event half-replaced,
    // and a failed validation can never lose data.
    g_server->on("/api/events/upsert", HTTP_POST, [&](AsyncWebServerRequest *req)
                 {
        const AsyncWebParameter* p = req->getParam("body", true);
        if (!p) { req->send(400, "text/plain", "Missing body"); return; }

        JsonDocument doc;
        if (deserializeJson(doc, p->value())) {
            req->send(400, "text/plain", "Invalid JSON");
            return;
        }
        JsonObject in = doc.as<JsonObject>();
        if (isBlankName(in["name"] | "")) {
            // The Settings page blocks this itself; this is the backstop for
            // any other client. The page shows this text in its toast.
            req->send(400, "text/plain", "Please enter a name - who is this event for?");
            return;
        }
        PersonalEvent ev = parseEventFields(in);
        autoFillMissingCalendarSide(ev);
        const char* name = ev.name;

        JsonDocument respDoc;
        JsonArray warnings = respDoc["warnings"].to<JsonArray>();

        // A missing Hebrew month (e.g. legacy data saved before the
        // front-end required one) is caught here directly, since
        // validateHebrewGregorianCombination() below only runs - and would
        // only catch it as a "dates don't match" mismatch - when
        // hasGregorian is ALSO set. A Hebrew-only event with no month would
        // otherwise pass through silently.
        if (ev.hasJewish && (ev.jMonth < 1 || ev.jMonth > 13)) {
            JsonObject w = warnings.add<JsonObject>();
            w["name"] = name;
            w["detail"] = "Hebrew date is missing its month - please edit and re-select it.";
        }

        String detail;
        if (!validateHebrewGregorianCombination(ev, detail)) {
            JsonObject w = warnings.add<JsonObject>();
            w["name"] = name;
            w["detail"] = detail;
        }

        // A client-provided id that no longer matches any live event (e.g.
        // it was deleted elsewhere since the edit form was opened) is NOT
        // silently treated the same as "no id" - idNotFound tells the
        // caller their edit was saved as a new event rather than
        // overwriting what they thought they were editing.
        String clientId = in["id"] | "";
        int existingIdx = -1;
        EventsLock::take();
        if (clientId.length()) {
            for (size_t i = 0; i < g_events->size(); i++) {
                if (strcmp((*g_events)[i].id, clientId.c_str()) == 0) { existingIdx = (int)i; break; }
            }
        }
        bool idNotFound = clientId.length() > 0 && existingIdx < 0;

        if (existingIdx >= 0) {
            strncpy(ev.id, clientId.c_str(), sizeof(ev.id) - 1);
            (*g_events)[existingIdx] = ev;
        } else {
            // New event (or a stale/unknown id): growing the vector touches
            // PSRAM, which - unlike MAX_EVENTS - is a real, shared, finite
            // resource (see its threshold comments in Config.h), so check
            // it before committing to the growth.
            size_t freeSpiram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t totalSpiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
            float freePct = totalSpiram ? (100.0f * freeSpiram / totalSpiram) : 100.0f;

            if (freePct < EVENTS_PSRAM_REFUSE_PCT) {
                EventsLock::give();
                JsonObject w = warnings.add<JsonObject>();
                w["name"] = name;
                w["detail"] = "Not saved: only " + String(freePct, 1) + "% of PSRAM free (refusing new events below " + String(EVENTS_PSRAM_REFUSE_PCT, 0) + "%).";
                respDoc["saved"] = false;
                sendJson(req, respDoc);
                return;
            }
            if (freePct < EVENTS_PSRAM_WARN_PCT) {
                JsonObject w = warnings.add<JsonObject>();
                w["name"] = name;
                w["detail"] = "Saved, but only " + String(freePct, 1) + "% of PSRAM remains free.";
            }

            strncpy(ev.id, Storage::generateEventId(*g_events).c_str(), sizeof(ev.id) - 1);
            // The PSRAM_REFUSE_PCT check above is a point-in-time heuristic,
            // not a hard guarantee against fragmentation - PsramAllocator
            // (PsramVector.h) still throws std::bad_alloc if growth genuinely
            // fails despite it passing.
            try {
                g_events->push_back(ev);
            } catch (const std::bad_alloc&) {
                EventsLock::give();
                req->send(400, "text/plain", "Not saved: ran out of PSRAM growing the events list.");
                return;
            }
        }

        bool saved = Storage::saveEvents(*g_events);
        EventsLock::give();
        if (!saved) {
            req->send(500, "text/plain", "Failed to save events");
            return;
        }
        removeUnusedImages();   // the picture this event had before, if nothing else uses it
        respDoc["saved"] = true;
        respDoc["id"] = ev.id;
        respDoc["idNotFound"] = idNotFound;
        sendJson(req, respDoc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/wifi/connect (POST {"network": 1..3}) ───────────────────────────
    // "Connect" on the page: join that known network now (it then becomes
    // the one tried first). The reply goes out before the move; if it's
    // another network, the page loses the clock until it's on that one too.

    g_server->on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *req) {
        String body;
        JsonDocument doc;
        if (!getBody(req, body) || deserializeJson(doc, body)) { req->send(400, "text/plain", "Missing body"); return; }
        int slot = (doc["network"] | 0) - 1;
        if (slot < 0 || slot >= WIFI_NETWORKS || !wifiSsidAt(*g_cfg, slot)[0]) { req->send(400, "text/plain", "No such network"); return; }
        WiFiManager::requestConnect(slot);
        req->send(200, "text/plain", String("Connecting to \"") + wifiSsidAt(*g_cfg, slot) + "\"..."); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/wifi/networks (GET, ?rescan=1) ──────────────────────────────────
    // Networks heard on the air, for "Add/Change network": {"status":
    // "scanning"} until a list is ready (ask again), then {"status":"done",
    // "networks":[{ssid,rssi,secure}...]}, strongest first. See
    // WiFiManager::heardNetworks().

    g_server->on("/api/wifi/networks", HTTP_GET, [](AsyncWebServerRequest *req) {
        std::vector<WiFiManager::HeardNetwork> heard;
        JsonDocument doc;
        if (!WiFiManager::heardNetworks(heard, req->hasParam("rescan"))) {
            doc["status"] = "scanning";
        } else {
            doc["status"] = "done";
            JsonArray arr = doc["networks"].to<JsonArray>();
            for (const auto& h : heard) {
                JsonObject o = arr.add<JsonObject>();
                o["ssid"] = h.ssid;
                o["rssi"] = h.rssi;
                o["secure"] = h.secure;
            }
        }
        sendJson(req, doc); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/wifi/forget (POST) ───────────────────────────────────────────────

    g_server->on("/api/wifi/forget", HTTP_POST, [&](AsyncWebServerRequest *req)
                 {
        CLOCK_LOG("WebUI", "Forgetting Wi-Fi credentials (all networks) and rebooting...");
        for (int i = 0; i < WIFI_NETWORKS; i++) {
            wifiSsidAt(*g_cfg, i)[0]     = '\0';
            wifiPasswordAt(*g_cfg, i)[0] = '\0';
        }
        g_cfg->wifiLastNetwork = -1;
        // Their places too. Directly, not via Location::forgetPlace(): the
        // clock restarts right after this reply, before the main loop would.
        memset(g_cfg->places, 0, sizeof(g_cfg->places));
        Storage::saveConfig(*g_cfg);
        req->send(200, "text/plain", "Wi-Fi credentials cleared. Rebooting into setup mode...");
        scheduleRestart(RESTART_AFTER_REPLY_MS); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/backup (GET) ─────────────────────────────────────────────────────

    g_server->on("/api/backup", HTTP_GET, [](AsyncWebServerRequest *req)
                 {
        CLOCK_LOG("WebUI", "Serving backup file...");
        flushConfigSave("before backup");   // the backup reads config.json from flash
        // Write to a LittleFS temp file and stream that, instead of handing a
        // String to beginResponse() - see the DiagnosticLog NUL-truncation
        // gotcha in CLAUDE.md. Same bug class, same fix shape: File::print(String)
        // writes String::length() bytes (not strlen), so an embedded NUL in any
        // config/event field survives intact instead of silently truncating the
        // download. Also avoids holding the backup as a 4th/5th in-RAM copy.
        const char* tmpPath = "/backup_tmp.json";
        String backup = Storage::exportBackup();
        File f = LittleFS.open(tmpPath, "w");
        if (!f) { req->send(500, "text/plain", "Backup write failed"); return; }
        f.print(backup);
        f.close();
        AsyncWebServerResponse *res = req->beginResponse(LittleFS, tmpPath, "application/json; charset=utf-8");
        res->addHeader("Content-Disposition", "attachment; filename=\"jewishclock-backup.json\"");
        req->send(res); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── /api/restore (POST) ───────────────────────────────────────────────────

    g_server->on("/api/restore", HTTP_POST, [](AsyncWebServerRequest *req)
                 {
        CLOCK_LOG("WebUI", "Restoring backup...");
        String body;
        if (!getBody(req, body)) { req->send(400, "text/plain", "Missing body"); return; }

        if (!Storage::importBackup(body)) {
            req->send(400, "text/plain", "Invalid backup format");
            return;
        }
        req->send(200, "text/plain", "Backup restored. Rebooting...");
        scheduleRestart(RESTART_AFTER_REPLY_MS); }).setFilter(ON_ANY_INCL_SETUP_FILTER);

    // ── OTA ───────────────────────────────────────────────────────────────────
    // Reachable from the synchronized AP too, same as every other route here
    // (ON_STA_OR_SYNCED_AP_FILTER) - gated by the AP password, same trust
    // boundary as the home Wi-Fi itself. The actual isolation boundary for
    // the pure setup-only portal (before Wi-Fi is configured at all) is
    // ON_AP_FILTER's usage in the portal routes, not this.

    g_server->on(
        "/update", HTTP_POST,
        [](AsyncWebServerRequest *req)
        {
            // Plain firmware .bin or a .jcupdate package - see Updater.h.
            bool ok = Updater::uploadEnd();
            req->send(ok ? 200 : 400, "text/plain", Updater::uploadMessage());
            if (ok) scheduleRestart(RESTART_AFTER_REPLY_MS); // not delay()+restart here: see scheduleRestart()
        },
        [](AsyncWebServerRequest *req, String filename,
           size_t index, uint8_t *data, size_t len, bool final)
        {
            if (!index) Updater::uploadBegin(filename, req->contentLength());
            Updater::uploadChunk(data, len);
        }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // Built-in fallback update page (compiled into the firmware), so an update
    // can always be installed even if the web files on the clock are broken.
    g_server->on("/update-safe", HTTP_GET, [](AsyncWebServerRequest *req)
                 { req->send(200, "text/html", Updater::EMERGENCY_UPDATE_PAGE); }).setFilter(ON_STA_OR_SYNCED_AP_FILTER);

    // ── Catch-all (MUST BE LAST) ──────────────────────────────────────────────

    g_server->onNotFound([](AsyncWebServerRequest *req)
                         {
        // Once Wi-Fi is up (STA or the synchronized AP - see ON_STA_OR_SYNCED_AP_FILTER
        // above), return 404 for unknown API calls instead of redirecting them into the
        // SPA-routing fallback below; everything else (including the bare "/" itself)
        // falls through to that fallback.
        if (req->url().startsWith("/api/") && ON_STA_OR_SYNCED_AP_FILTER(req)) {
            req->send(404, "text/plain", "API Endpoint Not Found");
        } else {
            req->redirect("/");
        } });

    initialized = true;
    CLOCK_LOG("WebUI", "Routes registered.");
}

// ─── Lifecycle Control ────────────────────────────────────────────────────────

AsyncWebServer* WebUI::getAsyncServer()
{
    if (g_server == nullptr)
        g_server = new AsyncWebServer(80);
    return g_server;
}

// Binds the HTTP listener - nothing else. Called from WiFiManager's
// startAP() (once per boot - it's a no-op on later reconnects, see
// if (g_apActive) return there) and startAPPortal() (pure AP setup mode).
// Deliberately knows nothing about mDNS: mDNS is purely an STA-side concern
// (see scheduleMdnsRecheck()), and coupling it to this AP/listener-startup
// function used to make the trigger timing follow AP lifecycle instead of
// STA lifecycle, which is what caused a real regression - see
// scheduleMdnsRecheck()'s comment.
//
// Used to unconditionally tear the listener down and rebuild it (via
// stopServer()) on every single call, which under frequent reconnects
// (poor signal) stacked into a multi-step synchronous chain that closed the
// HTTP socket for a real window - traced during the 2026-09-30
// networking-reliability investigation to progressively worsening
// responses and eventual complete unresponsiveness. Fixed by making this
// idempotent: a cheap no-op past the first call.
void WebUI::startServer(AppConfig &cfg)
{
    g_cfg = &cfg;
    // begin() (which needs the events list too, not just cfg) always runs
    // before this - see main.cpp's module init order comment - so routes
    // already exist by now. This is just a trip-wire for that invariant
    // ever breaking, not a real recovery path.
    if (g_server == nullptr) {
        CLOCK_LOG("WebUI", "[ERROR] startServer() called before WebUI::begin() - routes not registered");
        return;
    }

    // Bind the HTTP listener exactly once. The AP interface keeps the server
    // reachable regardless of STA state, so there's no functional reason to
    // bounce the socket just because STA reconnected.
    if (!g_serverStarted) {
        g_server->begin();
        g_serverStarted = true;
        CLOCK_LOG("WebUI", "HTTP server started.");
    }
}

// Investigated 2026-10-08: MDNS.begin() can report success while the
// responder still doesn't actually answer real network queries (observed
// directly - a fresh boot logged "MDNS.begin() OK" yet jewishclock.local
// stayed unresolvable from another machine on the same network for the
// rest of that boot). The pre-2026-10-07 code self-healed this by brute
// force: it called performMdnsInit() unconditionally on every single STA
// reconnect. That got removed as part of fixing the reconnect-storm bug
// above, but nothing replaced its self-healing property - and because this
// function is only ever reached once per boot (see startServer()'s
// comment), a bad first attempt now had no way to ever get retried short of
// the Tier 2 watchdog's MDNS_STALE_MS heartbeat-stale window. Fix: call this
// directly from WiFiManager's STA_CONNECTED case (every reconnect, not just
// the first), independent of startAP()'s once-per-boot gate. This keeps
// today's real fix (never block the hot path, never thrash indefinitely
// under sustained poor signal - see MDNS_FORCE_RETRY_ATTEMPTS, which bounds
// how many times a reconnect can force a real reinit regardless of what
// g_mdnsRunning claims) while restoring a genuine retry opportunity on each
// of the first few reconnects, matching the old code's reliability for the
// case that actually broke, without reintroducing unconditional-forever
// reinit on every single reconnect.
void WebUI::scheduleMdnsRecheck(const String &mdnsHost)
{
    bool hasStaConnection = (WiFi.getMode() == WIFI_STA || WiFi.getMode() == WIFI_AP_STA)
        && WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0);

    if (hasStaConnection && (!g_mdnsRunning || g_mdnsInitAttempts < MDNS_FORCE_RETRY_ATTEMPTS)) {
        g_pendingMdnsHost = mdnsHost;
        g_mdnsRecheckAtMs = millis() + 500;
        CLOCK_LOG("WebUI", "[MdnsDiag] mDNS recheck scheduled for host='%s' at +500ms (attemptsSoFar=%u)", mdnsHost.c_str(), g_mdnsInitAttempts);
    } else {
        CLOCK_LOG("WebUI", "[MdnsDiag] mDNS recheck NOT scheduled: hasStaConnection=%d g_mdnsRunning=%d attemptsSoFar=%u", hasStaConnection, g_mdnsRunning, g_mdnsInitAttempts);
    }
    // Pure AP mode (WIFI_AP, no STA): mDNS is intentionally never started -
    // unchanged from previous behavior.
}

// Not currently called anywhere (startServer() no longer tears the listener
// down to rebuild it - see above). Kept as a deliberate, explicit full
// teardown for any future caller that genuinely needs one (e.g. a manual
// "restart networking" debug action), rather than removed outright.
void WebUI::stopServer()
{
    if (g_mdnsRunning) {
        MDNS.end();
        g_mdnsRunning = false;
    }
    if (g_server && g_serverStarted) {
        g_server->end();
        g_serverStarted = false;
        CLOCK_LOG("WebUI", "Server and mDNS stopped.");
    }
}

// ─── loop() ──────────────────────────────────────────────────────────────────

void WebUI::loop()
{
    unsigned long now = millis();

    static uint32_t factoryResetAt = 0;
    if (g_factoryResetRequested.exchange(false)) factoryResetAt = now + RESTART_AFTER_REPLY_MS;
    if (factoryResetAt && (int32_t)(now - factoryResetAt) >= 0) {
        g_cfgDirty = false;   // the old settings must not be written back
        Storage::factoryReset();
        WiFi.disconnect(true, true);   // the Wi-Fi driver's own stored network too
        delay(200);
        ESP.restart();
    }

    uint32_t restartAt = g_restartAtMs;
    if (restartAt && (int32_t)(now - restartAt) >= 0) {
        flushConfigSave("before restart");
        CLOCK_LOG("WebUI", "Performing requested restart...");
        ESP.restart();
    }

    if (g_cfgDirty && now - g_cfgChangedAtMs >= CONFIG_SAVE_QUIET_MS) flushConfigSave("after the quiet period");

    // ── Diagnostic heartbeat (HEARTBEAT_LOG_MS, regardless of connection state) ──
    static unsigned long lastHeartbeat = 0;
    if (lastHeartbeat == 0 || now - lastHeartbeat >= HEARTBEAT_LOG_MS) {
        lastHeartbeat = now;
        DiagnosticLog::record("HEARTBEAT",
            "wifiMode=%d rssi=%d heap=%u discon=%u recon=%u apRebuild=%u mdnsRunning=%d",
            (int)WiFi.getMode(), WiFi.RSSI(), ESP.getFreeHeap(),
            WiFiManager::getDisconnectCount(), WiFiManager::getReconnectCount(),
            WiFiManager::getApRebuildCount(), g_mdnsRunning ? 1 : 0);
    }

    // ── Weak-signal episodes ─────────────────────────────────────────────────
    // Signal strength is set by placement and the router, not firmware, so
    // the useful thing to do is record WHEN it's weak and for how long. Logs
    // once when the signal drops below WEAK and once when it recovers above
    // RECOVERED (the gap avoids a log line per flicker around one threshold),
    // with the duration and the lowest reading. Sampled every WIFI_RSSI_SAMPLE_MS.
    static unsigned long lastRssiCheck = 0;
    static bool weakSignal = false;
    static unsigned long weakSince = 0;
    static int weakLowest = 0;
    if (now - lastRssiCheck >= WIFI_RSSI_SAMPLE_MS) {
        lastRssiCheck = now;
        if (WiFi.status() == WL_CONNECTED) {
            int rssi = WiFi.RSSI();
            if (!weakSignal && rssi < WIFI_RSSI_WEAK_DBM) {
                weakSignal = true;
                weakSince = now;
                weakLowest = rssi;
                DiagnosticLog::record("WIFI", "signal weak: rssi=%d dBm", rssi);
            } else if (weakSignal) {
                if (rssi < weakLowest) weakLowest = rssi;
                if (rssi > WIFI_RSSI_RECOVERED_DBM) {
                    weakSignal = false;
                    DiagnosticLog::record("WIFI", "signal recovered: rssi=%d dBm after %lus weak (lowest %d dBm)",
                                          rssi, (now - weakSince) / 1000, weakLowest);
                }
            }
        }
    }

    // CLOCK_LOG("WebUI", "WebUI::loop: WiFi status = %d", WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {

        if (g_mdnsBeginAtMs != 0 && (long)(now - g_mdnsBeginAtMs) >= 0) {
            g_mdnsBeginAtMs = 0;
            finishMdnsInit();
        }

        // ── Reconnect-triggered fast path ─────────────────────────────────────
        // Services the deferred request scheduleMdnsRecheck() makes on every
        // (re)connect (see its comment, and WiFiManager's STA_CONNECTED case).
        // Runs at most ~500ms after the request, on a loop() tick separate
        // from whichever tick actually handled the reconnect - so the
        // ~250ms+ MDNS.end()/begin() cost here can never stack onto the same
        // synchronous chain as the AP rebuild and HTTP server (re)bind.
        if (g_mdnsRecheckAtMs != 0 && (long)(now - g_mdnsRecheckAtMs) >= 0) {
            g_mdnsRecheckAtMs = 0;
            if (!g_mdnsRunning || g_mdnsInitAttempts < MDNS_FORCE_RETRY_ATTEMPTS) {
                g_diagMdnsTier2FromServer++;
                performMdnsInit(g_pendingMdnsHost.c_str());
            }
        }

        // ── Tier 2: Recovery health check (every MDNS_WATCHDOG_CHECK_MS) ──────
        // Checks two conditions that each justify a full restart:
        //   a) Daemon not running at all (initial failure or crash)
        //   b) Tier 1 alive-timestamp older than MDNS_STALE_MS (the Tier 1
        //      refresh interval + a grace period), meaning Tier 1 silently failed.
        if (g_mdnsBeginAtMs == 0 && now - g_lastMdnsHealthCheckMs >= MDNS_WATCHDOG_CHECK_MS) {
            g_lastMdnsHealthCheckMs = now;
            // CLOCK_LOG("WebUI", "mDNS Watchdog: Performing check. g_mdnsRunning=%d, lastMdnsAliveAt=%lu", g_mdnsRunning, g_lastMdnsAliveAt);

            bool daemonDead  = !g_mdnsRunning;
            bool heartbeatStale = g_mdnsRunning && (now - g_lastMdnsAliveAt > MDNS_STALE_MS);

            // CLOCK_LOG("WebUI", "mDNS Watchdog: daemonDead=%d, heartbeatStale=%d", daemonDead, heartbeatStale);

            if (daemonDead || heartbeatStale) {
                // CLOCK_LOG("WebUI", "mDNS Watchdog: %s — invoking Tier 2 recovery",
                //           daemonDead ? "daemon dead" : "heartbeat stale");
                g_diagMdnsTier2FromWatchdog++;
                performMdnsInit(StringTools::makeMdnsHostname(g_cfg->projectName).c_str());
                g_lastTier1Refresh = now; // Sync heartbeat timer to avoid immediate Tier 1
            }
        }

        // ── Tier 1: Lightweight Keep-alive (every MDNS_REFRESH_MS) ───────────
        // Only runs when daemon is confirmed healthy.
        // Refreshes network caches without goodbye packets or task teardown.
        if (g_mdnsRunning && (now - g_lastTier1Refresh >= MDNS_REFRESH_MS)) {
            // CLOCK_LOG("WebUI", "mDNS Tier 1: Triggering refresh");
            g_lastTier1Refresh = now;
            refreshMdnsServices();
        }

    } else {
        // WiFi disconnected — mark daemon dead and reset all timers.
        // performMdnsInit guards against 0.0.0.0 anyway, but being explicit
        // here ensures the Tier 2 health check triggers a clean restart on reconnect.
        if (g_mdnsRunning) {
            MDNS.end();
            g_mdnsRunning = false;
            // CLOCK_LOG("WebUI", "mDNS: WiFi lost — daemon stopped, will recover on reconnect");
        }
        g_lastMdnsAliveAt   = 0;
        g_lastTier1Refresh  = 0;
        g_lastMdnsHealthCheckMs = 0;
    }

    // ── Dev helper: keep FE build time metadata current ──────────────────────
    static unsigned long lastFeRefresh = 0;
    if (now - lastFeRefresh >= FE_BUILD_TIME_REFRESH_MS) {
        lastFeRefresh = now;
        refreshFeBuildTime();
    }
}