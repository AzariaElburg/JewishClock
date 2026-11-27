// =============================================================================
// Calendar.cpp
// JewishClock — Jewish calendar engine.
//
// Local arithmetic:  Hebrew date conversion (Dershowitz & Reingold, 4th ed.)
// Hebcal API:        Everything else — fetched, cached in LittleFS, parsed.
//
// Hebcal API is free, well-maintained, used by millions, and handles every
// postponement rule, Israel/Diaspora variant, and parasha doubling correctly.
// We fetch once per day (or on forced refresh) and cache to /hebcal_cache.json.
// =============================================================================

#include <mutex>
#include <climits>
#include "FileTools.h"
#include "Calendar.h"
#include "Uptime.h"
#include "DateRules.h"
#include "OnlineUpdate.h"
#include "EventsLock.h"
#include "NTPManager.h"
#include "Config.h"
#include "DiagnosticLog.h"
#include "Display.h"
#include "LogLevel.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "StringTools.h"
#include "GeolocationService.h"
#include "Location.h"
#include "WiFiManager.h"
#include <ArduinoJson.h>
#include "PsramJsonAllocator.h"
#include "JewishDateEngine.h"
#include "SunTimes.h"
#include "Tunables.h"
#include "Texts.h"
#include "NetHealth.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>

DayInfo       Calendar::s_today;
DayInfo       Calendar::s_civilToday;
std::vector<DayInfo> Calendar::s_manifest;
int64_t Calendar::s_lastFetchMs  = 0;
bool          Calendar::s_forceRefresh = false;
static std::atomic<bool> s_timeJumped{false};   // set from the web server's task - see Calendar::timeJumped()
// Failed main fetches in a row (0 = the last one worked) and when the last one failed - see retryWaitMs().
static unsigned       s_mainFails = 0;
static int64_t  s_lastMainFailMs = 0;
static std::atomic<bool> s_clearCacheRequested{false};   // likewise - see update() 1a
// A new main calendar not yet written to the cache file (saved after its zmanim).
static bool s_cacheUnsaved = false;
// A failed location lookup: which network, and when to try again (step 3).
static int s_lookupRetrySlot = -1;
static uint32_t s_lookupRetrySets = 0;   // Location::userSets() when the failed lookup was asked for
static int64_t s_lookupRetryAtMs = 0;
// Failing downloads, into the persistent log once per streak (DiagnosticLog::Streak).
static DiagnosticLog::Streak s_hebcalStreak, s_zmanimStreak, s_geoStreak;
// "HTTP 503" for a server's answer, the library's text for a connection failure (negative codes).
static String httpFailure(int code) { return code > 0 ? "HTTP " + String(code) : HTTPClient::errorToString(code); }

static const char* CACHE_PATH       = "/jewish_calendar_data.json";
static std::atomic<uint32_t> s_cacheFileHash{0};   // see writeCacheFile()
// Saved-calendar format. Raise it whenever DayInfo's fields or the parsing
// rules change, so an old file is re-downloaded instead of shown as is.
// 2 (2026-10-18): Chanukah by civil date, minor/modern holidays hidden.
// 3 (2026-10-18): a second holiday per day (holidayHebrew2).
// 4 (2026-10-18): Chanukah worded "חנוכה: נר א׳".
// 5 (2026-10-18): Chanukah's "8th Day" item no longer shown.
// 6 (2026-10-20): tzeit = צאת הכוכבים (tzeit7083deg) instead of dusk; Chanukah
//    from the Hebrew date (MessageAggregator), no longer kept here.
static const int   CACHE_FORMAT     = 6;

// === TUNABLES ===  (rule: see Config.h header)
// Minimum gap between main Hebcal fetches (a big monthly payload - no need
// to hammer it). Does NOT gate the zmanim retry below; see update() step 4.
static const int64_t MAIN_FETCH_COOLDOWN_MS   = 5LL * 60 * 1000;
TUNABLE_SHARED(MAIN_FETCH_COOLDOWN_MS)
// After a FAILED download (main calendar or zmanim) the next try waits
// RETRY_FIRST_MS, doubling with each failure in a row up to RETRY_MAX_MS
// (10, 20, 40, 60, 60... s): a short blip is over within seconds, a long outage
// costs one try a minute - retryWaitMs(). Always, even when a refresh is forced
// or the date jumped. Until 2026-11-11 only a success started a pause: with
// Hebcal down the clock asked again right after each failure (seen in the drills
// every 5 s - the connect timeout; a quick refusal would have been many a second).
// The user, 2026-11-11: growing waits, at most a minute (5 min was too long).
static const int64_t RETRY_FIRST_MS = 10LL * 1000;
TUNABLE_SHARED(RETRY_FIRST_MS)
static const int64_t RETRY_MAX_MS   = 60LL * 1000;
TUNABLE_SHARED(RETRY_MAX_MS)
// The calendar downloaded reaches this many days past today - or further when an event's
// reminder starts earlier (its warning days) - plus a week, so it never runs dry between downloads.
static const int    CALENDAR_DAYS_AHEAD_MIN = 14;
TUNABLE_SHARED(CALENDAR_DAYS_AHEAD_MIN)
static const int    CALENDAR_DAYS_MARGIN    = 7;
TUNABLE_SHARED(CALENDAR_DAYS_MARGIN)
// A failed location lookup for the connected network, tried again after this.
static const int64_t LOOKUP_RETRY_MS = 5LL * 60 * 1000;
TUNABLE_SHARED(LOOKUP_RETRY_MS)

// Tracks whether the Zmanim gap-fill (see applyZmanimPayload below) succeeded on the
// current manifest. A transient failure here (network hiccup, one bad response)
// must not leave "today" without a sunset/tzeit rollover trigger until the next
// scheduled monthly-range refetch (up to a day away) - so this is retried on its
// own short wait (retryWaitMs()), independent of the main manifest fetch's
// daily gate.
static bool          s_zmanimOk            = false;
static int64_t s_lastZmanimAttemptMs = 0;
static unsigned      s_zmanimFails         = 0;   // failed zmanim downloads in a row
static int64_t s_lastZmanimFailMs    = 0;

// The wait after `fails` failed downloads in a row (see RETRY_FIRST_MS).
static int64_t retryWaitMs(unsigned fails) {
    int64_t w = RETRY_FIRST_MS;
    for (unsigned i = 1; i < fails && w < RETRY_MAX_MS; i++) w *= 2;
    return w < RETRY_MAX_MS ? w : RETRY_MAX_MS;
}

// ─── Async HTTP GET helper ──────────────────────────────────────────────────
//
// WHY THIS EXISTS: investigated 2026-10-06. Both Hebcal calls below used to
// run HTTPClient+WiFiClientSecure GETs directly inline in Calendar::update(),
// which runs on the main Arduino loop() task - the SAME task that drives
// Display rendering and feeds the AsyncWebServer's in-progress responses.
// A single hebcal.com zmanim retry was measured, via restored per-step
// timing instrumentation (see main.cpp's TIMED_STEP), freezing that entire
// task for 2.75 seconds solid - zero Display/WebUI log activity the whole
// time. That's long enough on its own to abort an in-progress page transfer
// to a browser (seen as net::ERR_INCOMPLETE_CHUNKED_ENCODING) and is NOT a
// local WiFi quality issue - it's a round-trip to an external internet
// server (DNS + TLS handshake + request/response), which is inherently
// slower and less predictable than anything on the local network, blocking
// everything else on the device for its entire duration.
//
// Fix: run only the blocking network I/O on a small dedicated background
// task. Every other line of logic (JSON parsing, manifest building, caching,
// syncToday) stays exactly where it was, unchanged, on the main thread -
// only the slow part moves, which minimizes the risk of introducing a new
// bug versus a larger threading refactor.
//
// Single in-flight request at a time by design (Calendar never needs two
// concurrent external fetches), so no locking is needed beyond the busy/done
// flags themselves: the main thread only writes s_fetchUrl before signaling
// the task, and only reads s_fetchPayload/s_fetchHttpCode after observing
// s_fetchDone - the task notification and the done flag each provide the
// happens-before ordering needed for that handoff to be safe.
//
// 2026-10-24: the task also does the two other slow parts of a refresh, so
// the screen doesn't wait for them (measured on the display loop: parsing
// the main download 43 ms, writing the cache file ~155 ms - twice):
//  - a main-calendar download is parsed here into a finished list
//    (s_fetchManifest); the main loop only swaps it in;
//  - the cache file is written here from a snapshot copy (saveCache()).
static void parseManifestInto(const String& json, std::vector<DayInfo>& out);   // below
static void writeCacheFile(const std::vector<DayInfo>& days);                    // below
namespace {
    // std::atomic, not plain volatile: this flag is the cross-core handoff
    // signal between the fetch task and the main loop task, and volatile
    // alone does not guarantee visibility of the payload/httpCode writes
    // that happen-before it on ESP32's dual-core design.
    TaskHandle_t      s_fetchTaskHandle = nullptr;
    std::atomic<bool> s_fetchBusy{false};
    std::atomic<bool> s_fetchDone{false};
    std::atomic<bool> s_fetchWanted{false};   // a download is waiting for the task
    String        s_fetchUrl;
    // A server asked us to wait - HTTP 429/503 with Retry-After, or ip-api's own "none left" (X-Rl 0, the
    // window's end in X-Ttl s): no download from THAT host before (another host isn't held up). A few
    // hosts at most (Hebcal, the place lookup, Open-Meteo): the oldest hold makes room.
    struct HostHold { char host[48]; int64_t untilMs; };
    HostHold      s_holds[4] = {};
    std::mutex    s_holdsLock;   // the download task writes, the main loop reads
    String hostOf(const String& url) {
        int a = url.indexOf("//");
        String h = url.substring(a < 0 ? 0 : a + 2);
        int b = h.indexOf('/');
        return b < 0 ? h : h.substring(0, b);
    }
    void holdHost(const String& host, int64_t untilMs) {
        std::lock_guard<std::mutex> g(s_holdsLock);
        int slot = 0;
        for (int i = 0; i < 4; i++) {
            if (host.equalsIgnoreCase(s_holds[i].host)) { slot = i; break; }
            if (s_holds[i].untilMs < s_holds[slot].untilMs) slot = i;
        }
        if (!host.equalsIgnoreCase(s_holds[slot].host) || untilMs > s_holds[slot].untilMs) s_holds[slot].untilMs = untilMs;
        StringTools::utf8Copy(s_holds[slot].host, host.c_str());
    }
    bool hostHeld(const String& host) {
        bool held = false;
        std::lock_guard<std::mutex> g(s_holdsLock);
        for (auto& h : s_holds) if (host.equalsIgnoreCase(h.host) && Uptime::ms() < h.untilMs) held = true;
        return held;
    }
    String        s_fetchPayload;
    int           s_fetchHttpCode   = -1;
    bool          s_fetchParse = false;                      // parse it as the main calendar
    std::vector<DayInfo>* s_fetchManifest = nullptr;         // ... into this (handed over with s_fetchDone)
    std::atomic<std::vector<DayInfo>*> s_saveSnapshot{nullptr};   // a cache file to write

    void doFetch() {
            HTTPClient http;
            int code;
            // Hebcal needs HTTPS (cert check skipped, matching the original
            // inline code); GeolocationService's IP-API endpoint is plain
            // HTTP - forcing WiFiClientSecure onto it here was a real bug
            // caught during verification (silent TLS handshake failure
            // against a server not speaking TLS on that endpoint).
            // client must live until getString() below, not just until GET() -
            // http.begin(client, ...) only stores a reference to it. It used to
            // be declared inside the if-block, so it was destroyed the instant
            // that block ended, right after GET() returned - leaving http
            // holding a reference to an already-destroyed object by the time
            // getString() ran a few lines later. That's the actual root cause
            // of the "200 status, 0-byte body" failures investigated above:
            // fully deterministic (reading a dead connection every time), which
            // is exactly why none of the timeout/HTTP-version/core-pinning
            // changes made any difference - none of them were ever the real
            // mechanism. Confirmed new in the refactor that added the
            // HTTP-vs-HTTPS branch; the pre-refactor inline version had no
            // such branch and kept client alive in one flat scope throughout.
            WiFiClientSecure client;
            // A server's "come back in N s" (RFC 9110); ip-api's requests left in its minute and the
            // seconds to its end (ip-api.com/docs: 45 a minute, then 429 - and banned for an hour if it goes on)
            static const char* WAIT_HEADERS[] = { "Retry-After", "X-Rl", "X-Ttl" };
            if (s_fetchUrl.startsWith("https://")) {
                client.setInsecure();
                http.begin(client, s_fetchUrl);
                http.collectHeaders(WAIT_HEADERS, 3);
                code = http.GET();
            } else {
                http.begin(s_fetchUrl);
                http.collectHeaders(WAIT_HEADERS, 3);
                code = http.GET();
            }
            if (code > 0) NetHealth::heard();   // it answered: the internet is there
            // Too many / not now (2026-11-21, the user: act on what a server answers, always): Hebcal
            // answers 429 past 90 requests in 10 s - its Retry-After obeyed, else the retry's longest wait.
            bool noneLeft = http.hasHeader("X-Rl") && http.header("X-Rl").toInt() <= 0;   // ip-api: the minute used up
            if (code == 429 || code == 503 || noneLeft) {
                long waitS = http.header("Retry-After").toInt();
                if (waitS <= 0) waitS = http.header("X-Ttl").toInt() + 1;   // ip-api: its window's end
                if (waitS <= 1) waitS = (long)(RETRY_MAX_MS / 1000);
                if (waitS > 24L * 3600) waitS = 24L * 3600;
                String host = hostOf(s_fetchUrl);
                holdHost(host, Uptime::ms() + (int64_t)waitS * 1000);
                DiagnosticLog::record("Calendar", Texts::svcWait, host.c_str(), waitS, code);
            }
            String payload = (code == 200) ? http.getString() : String();
            http.end();
            if (s_fetchParse && code == 200 && payload.length() >= 50) {
                auto* days = new std::vector<DayInfo>();
                parseManifestInto(payload, *days);
                s_fetchManifest = days;
                payload = String();   // parsed - don't hand ~25 KB of text to the main loop too
            }
            s_fetchPayload  = payload;
            s_fetchHttpCode = code;
            s_fetchDone = true; // publish LAST - main thread only reads the above after seeing this
            s_fetchBusy = false;
    }

    void fetchTaskFn(void*) {
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // sleep until there's work
            // Is the internet there (a time server silent)? - the same task. First, and also
            // while a cache file waits below: the time waits on its answer ("at once", NetHealth.h;
            // 2026-11-22: it waited 11 s behind a cache file for a still screen - up to
            // FLASH_WRITE_MAX_WAIT_MS - and the next server was asked that much later).
            NetHealth::probeInTask();
            // A cache file, written while the screen stands still (a flash
            // write pauses it); downloads meanwhile go ahead, and a newer
            // copy replaces the waiting one.
            if (auto* snap = s_saveSnapshot.exchange(nullptr)) {
                int64_t since = Uptime::ms();
                while (!DisplayManager::goodMomentToWrite(since)) {
                    NetHealth::probeInTask();
                    if (s_fetchWanted.exchange(false)) doFetch();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    if (auto* newer = s_saveSnapshot.exchange(nullptr)) { delete snap; snap = newer; }
                }
                writeCacheFile(*snap);
                delete snap;
            }
            if (s_fetchWanted.exchange(false)) doFetch();
            OnlineUpdate::runInTask();   // a check / download asked for (OnlineUpdate.h) - the same one task
            NetHealth::probeInTask();    // asked for meanwhile: not one more round's wait
        }
    }

    void ensureTask();
}
void Calendar::wakeTask() { ensureTask(); xTaskNotifyGive(s_fetchTaskHandle); }
namespace {
    void ensureTask() {
        if (!s_fetchTaskHandle) {
            // 16 KB: the main-calendar parse now runs here too (ArduinoJson recursion).
            xTaskCreatePinnedToCore(fetchTaskFn, "calFetch", 16384, nullptr, 1, &s_fetchTaskHandle, 1);
        }
    }

    // Returns false if a fetch is already in flight - or finished with its result
    // not yet taken by update() step 1b; the caller tries again later.
    // parse: it's the main calendar - parsed by the task (s_fetchManifest).
    // (2026-11-14, the flaky "runs out, Hebcal down" drill: only s_fetchBusy was
    // checked. A download finishing between step 1b and step 4 of one update() -
    // busy cleared, result waiting - let step 4 start the next one, which cleared
    // s_fetchDone: the result was thrown away unseen. A failure then wasn't
    // counted (the next try 5 s later, no retry wait); a success was downloaded
    // again, and its parsed calendar leaked.)
    bool startAsyncFetch(const String& url, bool parse = false) {
        if (s_fetchBusy || s_fetchDone) return false;
        if (hostHeld(hostOf(url))) return false;   // that server asked us to wait: later
        ensureTask();
        s_fetchUrl  = url;
        s_fetchParse = parse;
        s_fetchManifest = nullptr;
        s_fetchDone = false;
        s_fetchBusy = true;
        s_fetchWanted = true;
        xTaskNotifyGive(s_fetchTaskHandle);
        return true;
    }
}

// What the currently in-flight (or just-completed) async fetch is for, so
// update() knows how to process s_fetchPayload once s_fetchDone is true.
enum class PendingFetch { None, MainManifest, ZmanimGapFill, GeoSync, PlaceSearch };
static PendingFetch s_pendingFetch = PendingFetch::None;
// The network an in-flight GeoSync lookup is for (Location::applyLookup()).
static int s_pendingLookupSlot = -1;
// Uptime::ms() when the in-flight GeoSync fetch was started, so the completion
// handler below can report its duration - GeolocationService no longer
// times this itself now that nothing in it makes the blocking call.
static int64_t s_geoSyncStartMs = 0;
// A lookup asked for (network slot), started by update() once no other
// download runs. Set from the main loop (Location) or the web task (Detect).
static std::atomic<int> s_lookupRequestSlot{-1};
static std::atomic<uint32_t> s_lookupRequestSets{0};   // Location::userSets() when it was asked for
static uint32_t s_pendingLookupSets = 0;

void Calendar::requestLookup(int slot) {
    if (slot < 0 || slot >= WIFI_NETWORKS) return;
    s_lookupRequestSets = Location::userSets(slot);
    s_lookupRequestSlot = slot;
    CLOCK_LOG("Calendar", "Location lookup requested for network %d.", slot + 1);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

// UTC date and time -> seconds since 1970 (timegm() isn't on every ESP32
// toolchain). The one day count, Config.h daysFromCivil() - until 2026-11-14
// its own, which counted 2100 as a leap year and read its month table at
// whatever month an answer gave. Callers pass DateRules-checked values.
static time_t utcSeconds(int y, int m, int d, int h, int mi, int s) {
    return (time_t)(daysFromCivil(y, m, d) * 86400 + h * 3600 + mi * 60 + s);
}

// The display font has no nikud (vowel point) glyphs at all - Hebcal
// vocalizes some Hebrew strings (e.g. month names inside the mevarchim
// announcement), and each unmapped nikud codepoint draws nothing while still
// eating a full glyph-width advance, leaving a visible gap where the mark
// should be. Strips every nikud/cantillation codepoint (the whole U+0590-
// 05CF sub-block) out of a UTF-8 string; Hebrew letters (U+05D0-05EA) and
// punctuation like geresh/gershayim (U+05F3-05F4) live outside that range
// and are untouched. Same logic this file already used just for the Omer
// count field, now shared by every raw Hebcal "hebrew" string.
// Hebcal spells Cheshvan חשון, which reads correctly only with its nikud
// (חֶשְׁוָן). Without nikud - as everything is shown on the clock - the
// standard spelling is חשוון (double vav). Safe to apply repeatedly: חשוון
// doesn't contain חשון, and מרחשון becomes מרחשוון, also correct. Same
// spelling as JewishDateEngine's month table.
static void fixMonthSpelling(String& s) {
    s.replace("חשון", "חשוון");
}

// Removes nikud from Hebcal's Hebrew text - and, since every Hebrew string
// from Hebcal passes through here, also fixes the Cheshvan spelling.
// The Makaf (U+05BE, the Hebrew hyphen: לך־לך, אחרי מות־קדושים) sits inside
// the nikud block but is punctuation, not a vowel - kept (the font has it).
// Dropping it turned "לך־לך" into "לךלך" (found by the simulator, 2026-10-17).
static String stripNikud(const char* src) {
    if (!src) return String();
    String clean;
    for (size_t i = 0; src[i]; i++) {
        uint8_t c = (uint8_t)src[i];
        if (c == 0xD6 && (uint8_t)src[i+1] == 0xBE) { clean += (char)c; clean += (char)src[++i]; continue; } // Keep Makaf
        if (c == 0xD6) { if (src[i+1]) i++; continue; } // Skip the rest of the D6 vowel/cantillation block
        if (c == 0xD7 && src[i+1] && (uint8_t)src[i+1] < 0x90) { i++; continue; } // Skip D7 vowel sub-block
        clean += (char)c;
    }
    fixMonthSpelling(clean);
    return clean;
}

// Hebcal gives Mishna Yomit references with plain-digit chapter:verse numbers
// (e.g. "כלים 24:15-16"), unlike Daf Yomi, which it already gematria-formats
// itself. Converts every digit run to gematria while leaving the Hebrew Seder
// name and the ':'/'-' separators untouched, e.g. "כלים 24:15-16" -> "כלים
// כ״ד:ט״ו-ט״ז". Handles cross-chapter ranges (e.g. "26:9-27:1") the same way,
// since it just replaces each digit run independently.
static String mishnaRefToGematria(const String& raw) {
    int i = 0;
    while (i < (int)raw.length() && !StringTools::asciiDigit(raw[i])) i++;
    if (i >= (int)raw.length()) return raw; // No digits found; leave as-is.

    String result = raw.substring(0, i);
    while (i < (int)raw.length()) {
        if (StringTools::asciiDigit(raw[i])) {
            int start = i;
            while (i < (int)raw.length() && StringTools::asciiDigit(raw[i])) i++;
            char gematria[16];
            JewishDateEngine::numToGematria(raw.substring(start, i).toInt(), gematria, sizeof(gematria));
            result += gematria;
        } else {
            result += raw[i];
            i++;
        }
    }
    return result;
}

// Hebcal's own Hebrew-language holiday titles append the Hebrew year as plain
// ASCII digits (e.g. "ראש השנה 5787"), unlike its "hdate" field which already
// gives proper gematria for the same day. Replace a trailing " <digits>" run
// with our own gematria conversion of hYear, without the thousands (as years
// are shown everywhere else - JewishDateEngine::yearToGematria). Leaves the
// string untouched if it doesn't end in a plain digit run (e.g. Rosh Chodesh
// titles, which have none).
static String gematrizeTrailingYear(const char* raw, int hYear) {
    String s = raw;
    int i = s.length();
    while (i > 0 && StringTools::asciiDigit(s[i - 1])) i--;
    if (i == (int)s.length() || i == 0 || s[i - 1] != ' ') return s;
    s.remove(i - 1);
    if (hYear > 0) {
        char yearBuf[24];
        JewishDateEngine::yearToGematria(hYear, yearBuf, sizeof(yearBuf));
        s += " ";
        s += yearBuf;
    }
    return s;
}

static void extractTime(const char* isoDateTime, char* buf, size_t len) {
    buf[0] = '\0';
    if (!isoDateTime || !*isoDateTime) return;

    int yr, mon, day, hr, min, sec;
    // Hebcal ISO format: "2024-04-12T16:45:00" - only a real date and time (DateRules.h)
    if (!DateRules::parseIsoDateTime(isoDateTime, yr, mon, day, hr, min, sec)) return;

    // localtime_r() converts epoch to local struct tm based on system TZ (POSIX string).
    time_t t_utc = utcSeconds(yr, mon, day, hr, min, sec);
    struct tm tm_local;
    localtime_r(&t_utc, &tm_local);

    snprintf(buf, len, "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
}

// Hebcal's Zmanim endpoint returns times already localized to the requested tzid
// (e.g. "2026-09-14T18:48:00+03:00" for Asia/Jerusalem) - unlike the main hebcal
// endpoint (queried with tzid=UTC), whose ISO times are genuinely UTC and need
// the UTC->local conversion extractTime() does. Parsing the wall-clock hour:minute
// straight out of the string is correct here; converting it again would double-apply
// the UTC offset.
static void extractLocalIsoTime(const char* isoDateTime, char* buf, size_t len) {
    buf[0] = '\0';
    if (!isoDateTime || !*isoDateTime) return;
    int yr, mon, day, hr, min, sec;
    if (!DateRules::parseIsoDateTime(isoDateTime, yr, mon, day, hr, min, sec)) return;
    snprintf(buf, len, "%02d:%02d", hr, min);
}

// Fills in `sunset`/`tzeit` for manifest entries that don't already have one from
// the main Hebcal feed (which only sends candle-lighting/havdalah - and therefore
// sunset/tzeit - on Erev Shabbat/Yom Tov). Without this, "Sunset"/"Tzeit" rollover
// modes silently behave like "Midnight" on every ordinary day, since syncToday()
// has no trigger time to compare against. One extra request per fetch cycle, using
// the same date range as the main fetch - not a per-day loop.
//
// Split into start/apply (see the Async HTTP GET helper above): this only builds
// the URL and kicks off the background fetch; applyZmanimPayload() below does the
// actual JSON parsing once the payload is ready, back on the main thread.
static bool startZmanimFetch(const AppConfig& cfg, const char* startStr, const char* endStr) {
    char url[256];
    snprintf(url, sizeof(url), "https://www.hebcal.com/zmanim?cfg=json&latitude=%.4f&longitude=%.4f&tzid=%s&start=%s&end=%s",
             cfg.latitude, cfg.longitude, cfg.tzid, startStr, endStr);
    if (!startAsyncFetch(url)) return false;
    CLOCK_LOG("Calendar", "Requesting Zmanim URL: %s", url);
    return true;
}

static bool applyZmanimPayload(const String& payload, int httpCode, std::vector<DayInfo>& manifest) {
    // Failures print live only - the persistent log gets one line per
    // streak (s_zmanimStreak in update()), not one per 60 s retry.
    if (httpCode != 200) {
        CLOCK_LOG("Calendar", "Zmanim fetch failed: %s", httpFailure(httpCode).c_str());
        return false;
    }
    // Only "sunset" and "tzeit7083deg" are needed out of the ~35 zman types this
    // endpoint returns - filtering keeps parsing cheap on the ESP32. Fast
    // begin/end times come from the main feed instead. tzeit7083deg is Hebcal's
    // צאת הכוכבים (three small stars, 7.08 deg), when the Hebrew day changes in
    // Tzeit mode (the user's father, 2026-10-20: "from צאת הכוכבים everything
    // follows the day that starts then"); until 2026-10-20 this was "dusk"
    // (6 deg, ~5 min earlier).
    JsonDocument filter;
    filter["times"]["sunset"]       = true;
    filter["times"]["tzeit7083deg"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (err) {
        CLOCK_LOGF("Calendar", Texts::zmanimParseError, err.c_str());
        return false;
    }
    JsonObject sunsetByDate = doc["times"]["sunset"].as<JsonObject>();
    // A 200 whose body has no sunset times (an error object, or a range the
    // endpoint refused) would otherwise count as success - and a success
    // stops the 60s retry, leaving every ordinary day with no sunset.
    if (sunsetByDate.isNull() || sunsetByDate.size() == 0) {
        CLOCK_LOGF("Calendar", Texts::zmanimNoSunset);
        return false;
    }
    for (JsonPair kv : sunsetByDate) {
        for (auto& d : manifest) {
            if (strcmp(d.gregorianDate, kv.key().c_str()) == 0) {
                if (!d.sunset[0]) extractLocalIsoTime(kv.value().as<const char*>(), d.sunset, 10);
                break;
            }
        }
    }
    JsonObject tzeitByDate = doc["times"]["tzeit7083deg"].as<JsonObject>();
    for (JsonPair kv : tzeitByDate) {
        for (auto& d : manifest) {
            if (strcmp(d.gregorianDate, kv.key().c_str()) == 0) {
                if (!d.tzeit[0]) extractLocalIsoTime(kv.value().as<const char*>(), d.tzeit, 10);
                break;
            }
        }
    }
    CLOCK_LOG("Calendar", "Zmanim gap-fill complete.");
    return true;
}

// Just the zmanim-derived fields, kept small so carrying them across a
// manifest rebuild doesn't need a second full copy of the manifest.
struct ZmanimCarry {
    char date[11];
    char sunset[10];
    char tzeit[10];
};

static std::vector<ZmanimCarry> takeZmanim(const std::vector<DayInfo>& manifest) {
    std::vector<ZmanimCarry> out;
    out.reserve(manifest.size());
    for (const auto& d : manifest) {
        ZmanimCarry z;
        StringTools::utf8Copy(z.date, d.gregorianDate);
        StringTools::utf8Copy(z.sunset, d.sunset);
        StringTools::utf8Copy(z.tzeit, d.tzeit);
        out.push_back(z);
    }
    return out;
}

// Only fills fields the fresh parse left empty, so a value the main feed
// itself supplies (e.g. Friday's candle-lighting time as sunset) still wins.
static void restoreZmanim(const std::vector<ZmanimCarry>& carried, std::vector<DayInfo>& manifest) {
    for (auto& d : manifest) {
        for (const auto& z : carried) {
            if (strcmp(d.gregorianDate, z.date) != 0) continue;
            if (!d.sunset[0]) StringTools::utf8Copy(d.sunset, z.sunset);
            if (!d.tzeit[0])  StringTools::utf8Copy(d.tzeit, z.tzeit);
            break;
        }
    }
}

// ─── Calendar Logic ──────────────────────────────────────────────────────────

void Calendar::begin(const AppConfig& cfg) {
    memset(&s_today, 0, sizeof(s_today));
    StringTools::utf8Copy(s_today.hebrewDateStr, Texts::loading);
    s_manifest.clear();
    if (loadCache()) CLOCK_LOG("Calendar", "Manifest cache loaded (%u entries)", s_manifest.size());
    else CLOCK_LOG("Calendar", "Manifest cache empty or failed to load");
}

void Calendar::update(AppConfig& cfg, const EventList& events) {
    // 1. Performance Gate: Run logic once per second
    static int64_t lastUpdate = 0;
    if (Uptime::ms() - lastUpdate < 1000) return;
    lastUpdate = Uptime::ms();
    // Once-a-second lines: DEBUG level only (pure noise at INFO).
    if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "Update() running...");

    // 1a. What the web server asked for (clearCacheAndRefresh(), a location
    // lookup or place search) - done here, on this task, which is the only
    // one that reads and changes the calendar.
    if (s_clearCacheRequested.exchange(false)) {
        if (LittleFS.exists(CACHE_PATH)) {
            LittleFS.remove(CACHE_PATH);
            CLOCK_LOG("Calendar", "Cache file deleted via API.");
        }
        s_cacheFileHash = 0;
        s_manifest.clear();
        s_forceRefresh = true;
        s_lastFetchMs = 0; // Reset fetch timer to allow immediate fetch
        CLOCK_LOG("Calendar", "Manifest cleared and refresh forced via API.");
    }
    // (Not while a finished download's result still waits for 1b below -
    // starting another would overwrite it.)
    int lookupSlot = s_lookupRequestSlot;
    if (lookupSlot >= 0 && WiFi.status() == WL_CONNECTED && !s_fetchBusy && !s_fetchDone && startAsyncFetch(GeolocationService::getSyncUrl())) {
        s_lookupRequestSlot = -1;
        s_pendingFetch = PendingFetch::GeoSync;
        s_pendingLookupSlot = lookupSlot;
        s_pendingLookupSets = s_lookupRequestSets;
        s_geoSyncStartMs = Uptime::ms();
        CLOCK_LOG("Calendar", "Location lookup for network %d started.", lookupSlot + 1);
    }
    String searchQuery;
    if (!s_fetchBusy && !s_fetchDone && WiFi.status() == WL_CONNECTED && Location::searchToStart(searchQuery)) {
        if (startAsyncFetch(Location::searchUrl(searchQuery))) {
            Location::searchStarted();
            s_pendingFetch = PendingFetch::PlaceSearch;
        }
    }

    // 1b. Process a completed background fetch, if any - see the Async HTTP
    // GET helper above. This is what keeps the slow network I/O off this
    // thread: everything below was previously done synchronously, inline,
    // right after the blocking HTTPClient call that's now been moved to the
    // background task.
    if (s_fetchDone) {
        String       payload  = s_fetchPayload;
        int          httpCode = s_fetchHttpCode;
        PendingFetch kind     = s_pendingFetch;
        s_fetchDone    = false;
        s_pendingFetch = PendingFetch::None;

        if (kind == PendingFetch::MainManifest) {
            CLOCK_LOG("Calendar", "HTTP GET completed with code: %d", httpCode);
            // A genuine response for this date range is multiple KB at minimum -
            // 50 bytes safely distinguishes "actually got data" from a
            // 200-with-near-empty-body case (status line came through but the
            // body didn't). Treated as a failure below: must NOT reset
            // s_lastFetchMs (would silently eat the retry cooldown) or saveCache()
            // (would overwrite a previously-good cache with nothing).
            // Parsed by the background task (a list, or nothing if the answer
            // was too short or unreadable) - the main loop only swaps it in.
            std::vector<DayInfo>* days = s_fetchManifest;
            s_fetchManifest = nullptr;
            if (httpCode == 200 && (!days || days->empty())) {
                CLOCK_LOGF("Calendar", Texts::hebcalUnusable);
                s_hebcalStreak.fail("Calendar", Texts::hebcalStreak, "HTTP 200 with an unusable answer");
                s_mainFails++;
                s_lastMainFailMs = Uptime::ms();
                delete days;
            } else if (httpCode == 200) {
                s_hebcalStreak.ok("Calendar", Texts::hebcalStreak);
                CLOCK_LOG("Calendar", "Fetch successful: %u days.", (unsigned)days->size());
                // The main feed carries no sunset/tzeit for ordinary days -
                // those come from the separate zmanim gap-fill. Rebuilding the
                // manifest used to wipe them until that follow-up request
                // landed, and if it never did, Sunset/Tzeit rollover silently
                // became Midnight (the Hebrew date switching hours late). Keep
                // them by date and put them back after the rebuild.
                std::vector<ZmanimCarry> carried = takeZmanim(s_manifest);
                s_manifest.swap(*days);
                delete days;
                restoreZmanim(carried, s_manifest);
                // Any day new to this window still needs the gap-fill. A success
                // from an earlier, now-replaced manifest must not suppress the
                // 60s retry if the chained request below fails to start.
                s_zmanimOk = false;
                s_forceRefresh = false;
                s_lastFetchMs  = Uptime::ms();
                s_mainFails = 0;
                syncToday(cfg); // Refresh s_today immediately after data arrives

                // Immediately chain the zmanim gap-fill for this same range,
                // same as before - just non-blocking now, so it lands a tick
                // or two later instead of inline. s_fetchBusy is guaranteed
                // false here (we just consumed the only in-flight fetch).
                // The cache file is written once, after the zmanim answer
                // (whatever it is) - not twice per refresh.
                s_cacheUnsaved = true;
                if (!s_manifest.empty() && startZmanimFetch(cfg, s_manifest.front().gregorianDate, s_manifest.back().gregorianDate)) {
                    s_pendingFetch = PendingFetch::ZmanimGapFill;
                    s_lastZmanimAttemptMs = Uptime::ms();
                    CLOCK_LOG("Calendar", "Zmanim gap-fill (chained after main fetch) requested in background.");
                } else {
                    saveCache();
                    s_cacheUnsaved = false;
                }
            } else {
                CLOCK_LOG("Calendar", "Hebcal fetch failed: %s", httpFailure(httpCode).c_str());
                s_hebcalStreak.fail("Calendar", Texts::hebcalStreak, httpFailure(httpCode).c_str());
                s_mainFails++;
                s_lastMainFailMs = Uptime::ms();
            }
        } else if (kind == PendingFetch::ZmanimGapFill) {
            s_zmanimOk = applyZmanimPayload(payload, httpCode, s_manifest);
            CLOCK_LOG("Calendar", "Zmanim result: %s", s_zmanimOk ? "OK" : "FAILED - will retry");
            if (s_zmanimOk) { s_zmanimStreak.ok("Calendar", Texts::zmanimStreak); s_zmanimFails = 0; }
            else {
                s_zmanimStreak.fail("Calendar", Texts::zmanimStreak, httpCode == 200 ? "unusable answer" : httpFailure(httpCode).c_str());
                s_zmanimFails++;
                s_lastZmanimFailMs = Uptime::ms();
            }
            if (s_zmanimOk) syncToday(cfg);
            // New zmanim, or a new main calendar still unsaved (its zmanim failed).
            if (s_zmanimOk || s_cacheUnsaved) { saveCache(); s_cacheUnsaved = false; }
        } else if (kind == PendingFetch::GeoSync) {
            GeolocationService::setLastSyncDurationMs((uint32_t)(Uptime::ms() - s_geoSyncStartMs));
            bool geoOk = (httpCode == 200) && Location::applyLookup(cfg, s_pendingLookupSlot, payload, s_pendingLookupSets);
            CLOCK_LOG("Calendar", "Location lookup: %s", geoOk ? "OK" : "FAILED - will retry");
            if (geoOk) s_geoStreak.ok("Calendar", Texts::geoStreak);
            else {
                s_geoStreak.fail("Calendar", Texts::geoStreak, httpCode == 200 ? "unusable answer" : httpFailure(httpCode).c_str());
                s_lookupRetrySlot = s_pendingLookupSlot;   // again in LOOKUP_RETRY_MS (step 3)
                s_lookupRetrySets = s_pendingLookupSets;
                s_lookupRetryAtMs = Uptime::ms() + LOOKUP_RETRY_MS;
            }
        } else if (kind == PendingFetch::PlaceSearch) {
            Location::searchDone(httpCode, payload);
        }
    }

    // 2. Immediate Data Refresh (Rollover & Sync)
    syncToday(cfg);

    // 3. A failed location lookup is tried again - a network with no place
    // must not stay on the previous one's. (No monthly re-lookup any more:
    // the place follows the network - Location.cpp.)
    if (s_lookupRetrySlot >= 0 && Uptime::ms() >= s_lookupRetryAtMs) {
        // not when the user has set its place meanwhile - theirs stays (2026-11-20)
        if (s_lookupRetrySlot == WiFiManager::connectedSlot() && Location::userSets(s_lookupRetrySlot) == s_lookupRetrySets)
            requestLookup(s_lookupRetrySlot);
        s_lookupRetrySlot = -1;
    }
    NTPManager::DateTime dt;
    if (!NTPManager::getDateTime(dt)) {
        CLOCK_LOG("Calendar", "Update() exiting: NTPManager::getDateTime() returned false.");
        return;
    }

    // 4. Rolling Fetch Logic
    int64_t nowMs = Uptime::ms();
    if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "Update: Checking fetch logic. s_forceRefresh=%d, lastFetchMs=%lld, nowMs=%lld", s_forceRefresh, (long long)s_lastFetchMs, (long long)nowMs);
    // This cooldown only throttles the main Hebcal fetch below (it's a big monthly
    // payload, no need to hammer it). It must NOT gate step 5's independent zmanim
    // retry - it used to be a bare `return`, which silently blocked step 5 for the
    // full cooldown after every main fetch, including the very first one after
    // boot. If that fetch's own zmanim call happened to fail (early boot is exactly
    // when WiFi/DNS are least settled), the retry had no chance to run until this
    // cooldown expired.
    bool mainFetchOnCooldown = (!s_forceRefresh && !s_timeJumped && s_lastFetchMs > 0 && (nowMs - s_lastFetchMs < MAIN_FETCH_COOLDOWN_MS))
                            || (s_mainFails > 0 && nowMs - s_lastMainFailMs < retryWaitMs(s_mainFails));

    if (!mainFetchOnCooldown && !s_fetchBusy) {
        s_timeJumped = false;   // checked once, below: downloads only if the new date is missing
        time_t nowUtc = NTPManager::getTime();
        int maxWarnDays = CALENDAR_DAYS_AHEAD_MIN;
        EventsLock::take();
        for (const auto& ev : events) {
            if (ev.warningDays > maxWarnDays) maxWarnDays = ev.warningDays;
        }
        EventsLock::give();

        // Calculate start (today) and end (window) strings - the LOCAL date, as syncToday() and the
        // screen take it (2026-11-21: the UTC date here asked "is yesterday there?" from local
        // midnight until 02:00/03:00 in Israel; the window covered it, but by chance).
        (void)nowUtc;
        time_t nowLocal = NTPManager::getLocalEpoch();
        char startStr[11], endStr[11];
        struct tm tmb;
        gmtime_r(&nowLocal, &tmb);   // getLocalEpoch(): local time counted as if UTC
        strftime(startStr, sizeof(startStr), "%Y-%m-%d", &tmb);

        time_t endLocal = nowLocal + (time_t)(maxWarnDays + CALENDAR_DAYS_MARGIN) * 86400;
        gmtime_r(&endLocal, &tmb);
        strftime(endStr, sizeof(endStr), "%Y-%m-%d", &tmb);

        // More robust check: trigger fetch if today is missing OR if the cache window is too short.
        bool todayExists = false;
        if (!s_manifest.empty()) {
            for (const auto& day : s_manifest) {
                if (day.gregorianDate[0] && strcmp(day.gregorianDate, startStr) == 0) {
                    todayExists = true;
                    break;
                }
            }
        }
        bool needsMoreData = s_manifest.empty() || !todayExists || (s_manifest.back().gregorianDate[0] && strcmp(s_manifest.back().gregorianDate, endStr) < 0);

        if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "Fetch check: needsMoreData=%d (empty: %d, todayExists: %d, lastDate: %s, endDate: %s), force=%d",
                  needsMoreData, s_manifest.empty(), todayExists, s_manifest.empty() ? "N/A" : s_manifest.back().gregorianDate, endStr,
                  s_forceRefresh);

        if ((needsMoreData || s_forceRefresh) && WiFi.status() == WL_CONNECTED && NTPManager::isSynced()) {
            char url[320];
            // Verified flag set: o=on (Omer), F=on (Daf Yomi), myomi=on (Mishna), s=on (Parasha), mf=on (Minor Fasts),
            // maj=on (major holidays - Rosh Hashanah, Yom Kippur, Pesach, Shavuot, Sukkot), min=on (minor holidays -
            // Chanukah, Purim, Tu BiShvat, Lag B'Omer), nx=on (Rosh Chodesh), mvch=on (Shabbat Mevarchim).
            // i=on/off selects Israel vs. Diaspora holiday scheduling and Torah-reading cycle, from cfg.inIsrael
            // (itself derived from geolocation/country - see GeolocationService.cpp and WebUI.cpp).
            // h=on is kept though empirically it changes nothing in the response (tested against a month spanning
            // weekdays/Fri/Sat/Rosh Hashanah/Sukkot with h=on, h=off, and omitted - byte-identical results each time).
            const char* diasporaFlag = cfg.inIsrael ? "on" : "off";
            snprintf(url, sizeof(url), "https://www.hebcal.com/hebcal?v=1&cfg=json&start=%s&end=%s&geo=pos&latitude=%.4f&longitude=%.4f&tzid=UTC&c=on&M=on&h=on&o=on&ss=on&d=on&myomi=on&mf=on&mod=on&s=on&F=on&maj=on&min=on&nx=on&mvch=on&i=%s&lg=he",
                     startStr, endStr, cfg.latitude, cfg.longitude, diasporaFlag);
            if (startAsyncFetch(url, true)) {   // true: the task parses it
                s_pendingFetch = PendingFetch::MainManifest;
                CLOCK_LOGF("Calendar", Texts::calFetching, url);
            }
        }
    }

    // 5. Zmanim gap-fill retry (see s_zmanimOk comment above) - always reachable,
    // regardless of the main fetch's cooldown, so a failed zmanim call recovers on
    // its own waits (retryWaitMs() after a failure) instead of being stuck behind
    // mainFetchOnCooldown. Never tried yet (s_lastZmanimAttemptMs == 0) = due now:
    // when the saved calendar covers today (no main fetch, so nothing chains the
    // gap-fill), the first attempt otherwise waited until 60 s after boot - and
    // after sunset the clock showed the previous Hebrew date and Omer count for
    // that minute (found 2026-10-18 by the simulator-vs-clock test). Not done but
    // no failure counted (a new calendar whose chained request didn't start):
    // RETRY_MAX_MS after the last attempt, as before 2026-11-11.
    bool zmanimDue = s_lastZmanimAttemptMs == 0
                  || (s_zmanimFails > 0 ? nowMs - s_lastZmanimFailMs >= retryWaitMs(s_zmanimFails)
                                        : nowMs - s_lastZmanimAttemptMs >= RETRY_MAX_MS);
    if (!s_zmanimOk && !s_manifest.empty() && !s_fetchBusy && WiFi.status() == WL_CONNECTED && NTPManager::isSynced()
        && zmanimDue) {
        char zStart[11], zEnd[11];
        StringTools::utf8Copy(zStart, s_manifest.front().gregorianDate);
        StringTools::utf8Copy(zEnd, s_manifest.back().gregorianDate);
        if (startZmanimFetch(cfg, zStart, zEnd)) {
            s_pendingFetch = PendingFetch::ZmanimGapFill;
            s_lastZmanimAttemptMs = nowMs;
            CLOCK_LOG("Calendar", "Retrying Zmanim gap-fill (%s to %s)...", zStart, zEnd);
        }
    }
}

// Local time minus UTC, in minutes, on a given date (at noon; the summer-time
// switch happens at night, so that holds for the evening too).
static int utcOffsetMinOn(int y, int m, int d) {
    long day = JewishDateEngine::gregorianToAbsolute(y, m, d) - JewishDateEngine::gregorianToAbsolute(1970, 1, 1);
    time_t noonUtc = (time_t)day * 86400 + 12 * 3600;
    struct tm lt;
    localtime_r(&noonUtc, &lt);
    long ltDay = JewishDateEngine::gregorianToAbsolute(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday)
               - JewishDateEngine::gregorianToAbsolute(1970, 1, 1);
    return (int)((ltDay - day) * 1440 + lt.tm_hour * 60 + lt.tm_min - 12 * 60);
}

// Sunset / tzeit for a date, computed here from the place (SunTimes.h), as
// local "HH:MM:00" like Hebcal's - for when the downloaded calendar has no
// time for today (none downloaded yet, e.g. after a factory reset; or the
// zmanim download hasn't arrived or failed). Within 3 s of Hebcal's own
// times, so the same minute nearly always. "" if the sun doesn't get that low
// before local midnight (far north in summer): then the date changes at midnight.
// Remembered per date and place: syncToday() runs every loop, and the ESP32
// does double-precision maths in software.
static void computeZman(int y, int m, int d, double lat, double lon, bool tzeit, char* out, size_t len) {
    struct Memo { int y = 0, m = 0, d = 0; double lat = 0, lon = 0; bool ok = false; double utcMin = 0; };
    static Memo memo[2];   // [0] sunset, [1] tzeit
    Memo& c = memo[tzeit];
    if (c.y != y || c.m != m || c.d != d || c.lat != lat || c.lon != lon) {
        c.ok = SunTimes::eveningUtcMin(y, m, d, lat, lon, tzeit ? SunTimes::TZEIT_DEG : SunTimes::SUNSET_DEG, c.utcMin);
        c.y = y; c.m = m; c.d = d; c.lat = lat; c.lon = lon;
    }
    out[0] = 0;
    if (!c.ok) return;
    int t = (int)lround(c.utcMin) + utcOffsetMinOn(y, m, d);
    if (t < 0 || t >= 1440) return;
    snprintf(out, len, "%02d:%02d:00", t / 60, t % 60);
}

// Shared core of both s_today and s_civilToday: which manifest entry is
// "today" under a given rollover mode - the entry matching todayDate itself,
// or the next one if `mode`'s trigger time (sunset/tzeit; Midnight never
// triggers) has already passed. Both getToday() (Hebrew date/holiday/parasha/
// etc, using cfg.rolloverMode) and getCivilToday() (candle/Havdalah, always
// RolloverMode::Midnight - see Calendar.h) resolve through this one place,
// rather than each re-implementing the same search-and-advance logic.
static DayInfo resolveDayForMode(const std::vector<DayInfo>& manifest, int currentIdx, const char* nowTime, RolloverMode mode,
                                 const char* calcSunset = "", const char* calcTzeit = "") {
    const DayInfo& day = manifest[currentIdx];
    bool pastRollover = false;
    if (mode != RolloverMode::Midnight) {
        const char* triggerTime = (mode == RolloverMode::Tzeit) ? day.tzeit : day.sunset;
        if (!triggerTime[0]) triggerTime = (mode == RolloverMode::Tzeit) ? calcTzeit : calcSunset;   // see computeZman()
        if (triggerTime[0]) pastRollover = (strcmp(nowTime, triggerTime) >= 0);
    }
    if (pastRollover && (currentIdx + 1 < (int)manifest.size())) {
        return manifest[currentIdx + 1];
    }
    return manifest[currentIdx];
}

void Calendar::syncToday(const AppConfig& cfg) {
    NTPManager::DateTime dt;
    if (!NTPManager::getDateTime(dt)) return;

    // Get current LOCAL date and time. The manifest is keyed by the calendar date each
    // day's info actually applies to (a local-time concept - Hebcal candle/havdalah/tzeit
    // strings are already converted to local time by extractTime()). Using gmtime_r() here
    // instead of the local calendar date made "today" flip over at UTC midnight rather than
    // local midnight/sunset - a fixed multi-hour-late rollover for any non-zero UTC offset.
    time_t nowLocal = NTPManager::getLocalEpoch();
    struct tm tmb;
    gmtime_r(&nowLocal, &tmb);
    char todayDate[11], nowTime[12];
    strftime(todayDate, sizeof(todayDate), "%Y-%m-%d", &tmb);
    strftime(nowTime, sizeof(nowTime), "%H:%M:%S", &tmb);
    // CLOCK_LOG("Calendar", "syncToday check. SysDate: %s, RolloverMode: %d", todayDate, cfg.rolloverMode);

    if (s_manifest.empty()) {
        CLOCK_LOG("Calendar", "[DataTrace] syncToday: Manifest is EMPTY. Cannot sync.");
    }

    int currentIdx = -1;
    for (size_t i = 0; i < s_manifest.size(); i++) {
        if (strcmp(s_manifest[i].gregorianDate, todayDate) == 0) {
            currentIdx = (int)i;
            break;
        }
    }
    // CLOCK_LOG("Calendar", "Manifest search result for %s: index %d", todayDate, currentIdx);

    // Today's sunset/tzeit computed here when the calendar has none - see computeZman().
    int y = tmb.tm_year + 1900, mo = tmb.tm_mon + 1, dd = tmb.tm_mday;
    const DayInfo* d = (currentIdx != -1) ? &s_manifest[currentIdx] : nullptr;
    char calcSunset[10] = "", calcTzeit[10] = "";
    if (!d || !d->sunset[0]) computeZman(y, mo, dd, cfg.latitude, cfg.longitude, false, calcSunset, sizeof(calcSunset));
    if (!d || !d->tzeit[0])  computeZman(y, mo, dd, cfg.latitude, cfg.longitude, true,  calcTzeit,  sizeof(calcTzeit));
    if (d) {
        s_today      = resolveDayForMode(s_manifest, currentIdx, nowTime, cfg.rolloverMode, calcSunset, calcTzeit);
        // Candle/Havdalah always resolve as if rolloverMode were Midnight,
        // regardless of what the user actually configured - see the comment
        // on getCivilToday() in Calendar.h for why.
        s_civilToday = resolveDayForMode(s_manifest, currentIdx, nowTime, RolloverMode::Midnight);
    } else {
        const char* trigger = (cfg.rolloverMode == RolloverMode::Tzeit)  ? calcTzeit
                            : (cfg.rolloverMode == RolloverMode::Sunset) ? calcSunset : "";
        computeLocalHebrew(y, mo, dd, trigger[0] && strcmp(nowTime, trigger) >= 0);
    }

    // Rebuild ONLY today's own display string per the showHebrewYear toggle.
    // s_manifest entries (and anything derived from them elsewhere, e.g.
    // personal events) keep their year always — this only affects the string
    // shown for today's date on the clock's own screen.
    HebrewDate hToday = { s_today.hYear, s_today.hMonth, s_today.hDay };
    JewishDateEngine::formatHebrewDate(hToday, s_today.hebrewDateStr, sizeof(s_today.hebrewDateStr), cfg.showHebrewYear);

    // Persist every Hebrew date change with what drove it, so a "switched
    // too late" report can be checked after the fact instead of guessed at.
    // "local-fallback" means today wasn't in the downloaded calendar: the date
    // is computed here, and changes at the computed sunset/tzeit ("calc").
    static int s_lastLoggedHDay = -1, s_lastLoggedHMonth = -1;
    if (s_today.hDay != s_lastLoggedHDay || s_today.hMonth != s_lastLoggedHMonth) {
        const DayInfo& base = (currentIdx != -1) ? s_manifest[currentIdx] : s_today;
        char ss[24], tz[24];   // "calc HH:MM:SS" = computed here (computeZman)
        if (base.sunset[0]) StringTools::utf8Copy(ss, base.sunset); else snprintf(ss, sizeof(ss), calcSunset[0] ? "calc %s" : "none", calcSunset);
        if (base.tzeit[0])  StringTools::utf8Copy(tz, base.tzeit);  else snprintf(tz, sizeof(tz), calcTzeit[0] ? "calc %s" : "none", calcTzeit);
        DiagnosticLog::record("Calendar", "Hebrew date -> %d/%d at %s (mode=%d, source=%s, today sunset=%s tzeit=%s)",
                              s_today.hDay, s_today.hMonth, nowTime, (int)cfg.rolloverMode,
                              (currentIdx != -1) ? "manifest" : "local-fallback", ss, tz);
        s_lastLoggedHDay = s_today.hDay;
        s_lastLoggedHMonth = s_today.hMonth;
    }
}

// Today without the downloaded calendar: the Hebrew date of local date
// y-m-d, or of the next day once past the evening's rollover.
void Calendar::computeLocalHebrew(int y, int m, int d, bool pastRollover) {
    long abs = JewishDateEngine::gregorianToAbsolute(y, m, d);
    for (int civil = 0; civil < 2; civil++) {
        // Use a local temporary to avoid clearing the global state momentarily
        DayInfo tmp;
        memset(&tmp, 0, sizeof(DayInfo));
        int hy, hm, hd;
        JewishDateEngine::absoluteToHebrew(abs + (!civil && pastRollover ? 1 : 0), hy, hm, hd);
        tmp.hYear = hy; tmp.hMonth = hm; tmp.hDay = hd;
        HebrewDate h = { hy, hm, hd };
        JewishDateEngine::formatHebrewDate(h, tmp.hebrewDateStr, sizeof(tmp.hebrewDateStr));
        // Candle/havdalah stay empty either way (no calendar entry).
        if (civil) s_civilToday = tmp; else s_today = tmp;
    }
}

// Runs on the background fetch task (see the fetch helper above): fills `out`,
// touches nothing shared - JewishDateEngine and the string helpers are pure.
static void parseManifestInto(const String& json, std::vector<DayInfo>& out) {
    // PSRAM-backed, not the default allocator: this is the one JsonDocument
    // in the app whose size genuinely scales with external data (a full
    // month's Hebcal response) rather than a small fixed shape - see
    // PsramJsonAllocator.h. NoMemory from PSRAM being full is already
    // handled by the existing `if (err)` check below, same as any other
    // parse failure.
    JsonDocument* doc = new JsonDocument(PsramJsonAllocator::instance());
    DeserializationError err = deserializeJson(*doc, json);
    if (err) {
        LOG_AT(LogLevel::WARN, "Calendar", "Hebcal manifest JSON parse error: %s", err.c_str());
        delete doc;
        return;
    }
    JsonArray items = (*doc)["items"].as<JsonArray>();
    if (items.isNull()) {
        delete doc;
        return;
    }
    CLOCK_LOG("Calendar", "Processing %u items from JSON...", items.size());
    int hebdateProblems = 0;   // Hebcal's Hebrew dates unreadable or not ours: the first one logged

    for (JsonObject item : items) {
        const char* dateIso = item["date"] | "";
        int y, m, d;
        if (!DateRules::parseIsoDate(dateIso, y, m, d)) continue;   // not a real date: nothing of it is used
        char dateKey[11];
        snprintf(dateKey, sizeof(dateKey), "%04d-%02d-%02d", y, m, d);

        int entryIdx = -1;
        for (size_t i = 0; i < out.size(); i++) {
            if (strcmp(out[i].gregorianDate, dateKey) == 0) { entryIdx = i; break; }
        }

        if (entryIdx == -1) {
            out.emplace_back();
            entryIdx = out.size() - 1;
            memset(&out[entryIdx], 0, sizeof(DayInfo));
            StringTools::utf8Copy(out[entryIdx].gregorianDate, dateKey);
            // The day's Hebrew date: our own calculation until Hebcal's "hebdate"
            // item for the day says otherwise (below) - Hebcal is the source of truth.
            HebrewDate h = JewishDateEngine::calculateHebrew(GregorianDate{y, m, d, false});
            out[entryIdx].hYear = h.year; out[entryIdx].hMonth = h.month; out[entryIdx].hDay = h.day;
            JewishDateEngine::formatHebrewDate(h, out[entryIdx].hebrewDateStr, sizeof(out[entryIdx].hebrewDateStr));
            out[entryIdx].isShabbat = (JewishDateEngine::gregorianToAbsolute(y, m, d) % 7 == 6);
        }

        // Shortcut reference (safe within this single iteration)
        DayInfo* entry = &out[entryIdx];

        if (Logging::enabled(LogLevel::DEBUG)) {
            String itemJson;
            serializeJson(item, itemJson);
            CLOCK_LOG("Calendar", "[Raw Hebcal Item] %s", itemJson.c_str());
        }

        const char* cat   = item["category"] | "";

        // Hebcal's Hebrew date of the day ("hebdate"; the date itself is in its
        // "hdate" field, "20 Tevet 5787"): its numbers win over our own calculation,
        // the wording stays ours (formatHebrewDate: חשוון, as on the rest of the screen).
        // A disagreement is logged - our engine also dates every event.
        if (strcmp(cat, "hebdate") == 0) {
            HebrewDate h;
            if (!JewishDateEngine::parseHebcalDate(item["hdate"] | "", h)) {
                if (!hebdateProblems++) LOG_AT(LogLevel::WARN, "Calendar", "Hebcal's Hebrew date for %s not readable: \"%s\"", dateKey, (const char*)(item["hdate"] | ""));
            } else {
                if ((h.year != entry->hYear || h.month != entry->hMonth || h.day != entry->hDay) && !hebdateProblems++)
                    LOG_AT(LogLevel::WARN, "Calendar", "Hebrew date of %s: Hebcal %d/%d/%d, own calculation %d/%d/%d - Hebcal's used",
                           dateKey, h.day, h.month, h.year, entry->hDay, entry->hMonth, entry->hYear);
                entry->hYear = h.year; entry->hMonth = h.month; entry->hDay = h.day;
                JewishDateEngine::formatHebrewDate(h, entry->hebrewDateStr, sizeof(entry->hebrewDateStr));
            }
        }
        else if (strcmp(cat, "candles") == 0) {
            extractTime(dateIso, entry->candleLightStr, 10);
            // Deliberate, not a sloppy stand-in for sunset: on Erev Shabbat/Yom
            // Tov the day effectively begins at candle lighting (Shabbat is
            // accepted then, ~20-40 min before actual sunset), so that's when
            // the clock should roll over to the next Hebrew date. The zmanim
            // gap-fill only fills an EMPTY sunset, so it never overwrites this.
            if (!entry->sunset[0]) StringTools::utf8Copy(entry->sunset, entry->candleLightStr);
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Candles [%s]: %s", dateKey, entry->candleLightStr);
        }
        else if (strcmp(cat, "havdalah") == 0) {
            extractTime(dateIso, entry->havdalahStr, 10);
            // Deliberate, like candle lighting for sunset above: after Shabbat
            // or Yom Tov the next day starts at Havdalah, not at the (earlier)
            // weekday צאת הכוכבים. The zmanim gap-fill never overwrites this.
            if (!entry->tzeit[0]) StringTools::utf8Copy(entry->tzeit, entry->havdalahStr);
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Havdalah [%s]: %s", dateKey, entry->havdalahStr);
        }
        else if (strcmp(cat, "zmanim") == 0) {
            // Hebcal's own fast times (Israel rules in i=on mode). title_orig is
            // the untranslated English title, so this match doesn't depend on lg=.
            // Tisha B'Av's begin item sits on the eve (its memo names the eve),
            // so the end item's memo wins as the fast's name when there is one.
            const char* orig = item["title_orig"] | "";
            String memo = stripNikud(item["memo"] | "");
            if (strcmp(orig, "Fast begins") == 0) {
                extractTime(dateIso, entry->fastBegin, sizeof(entry->fastBegin));
                if (!entry->fastName[0]) StringTools::utf8Copy(entry->fastName, memo.c_str());
            } else if (strcmp(orig, "Fast ends") == 0) {
                extractTime(dateIso, entry->fastEnd, sizeof(entry->fastEnd));
                StringTools::utf8Copy(entry->fastName, memo.c_str());
            }
        }
        else if (strcmp(cat, "omer") == 0) {
            // Prefer full count string, fall back to "Hebrew" label
            const char* src = item["omer"]["count"]["he"] | item["hebrew"] | "";
            StringTools::utf8Copy(entry->omerHebrew, stripNikud(src).c_str());

            // Day number from "title_orig" ("Omer 13"), which stays English
            // whatever lg= asks for. "title" is in the requested language -
            // Hebrew here ("י״ג בעומר", no digits) - so reading it left the
            // day at 0 and the Omer slide never showed (found 2026-10-18 by
            // the simulator's Omer-night test; the clock agreed).
            const char* title = item["title_orig"] | item["title"] | "";
            for (int i = 0; title[i]; i++) {
                if (StringTools::asciiDigit(title[i])) {
                    entry->omerDay = atoi(title + i);
                    break;
                }
            }

            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Omer [%s]: %s", dateKey, entry->omerHebrew);
        }
        else if (strcmp(cat, "parashat") == 0) {
            String parashaRaw = stripNikud(item["hebrew"] | "");
            // Hebcal's Hebrew name always starts "פרשת " (no API option drops it) - taken off here.
            if (parashaRaw.startsWith("פרשת ")) {
                parashaRaw.remove(0, strlen("פרשת "));
            }
            StringTools::utf8Copy(entry->parashaHebrew, parashaRaw.c_str());

            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Parasha [%s]: %s", dateKey, entry->parashaHebrew);
        }
        else if (strcmp(cat, "dafyomi") == 0) {
            StringTools::utf8Copy(entry->dafYomi, item["title"] | "");
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Daf [%s]: %s", dateKey, entry->dafYomi);
        }
        else if (strcmp(cat, "mishnayomi") == 0) {
            String raw = item["title"] | "";
            StringTools::utf8Copy(entry->mishnaYomit, mishnaRefToGematria(raw).c_str());
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Mishna [%s]: %s", dateKey, entry->mishnaYomit);
        }
        else if (strcmp(cat, "mevarchim") == 0) {
            StringTools::utf8Copy(entry->mevarchimHebrew, stripNikud(item["hebrew"] | "").c_str());
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Mevarchim [%s]: %s", dateKey, entry->mevarchimHebrew);
        }

        // Religious Flags
        // "fast" and "ss" (special Shabbat) are SUBcategories of category=="holiday"
        // in Hebcal's response, not top-level categories like "candles"/"roshchodesh"
        // above - item["category"] is always "holiday" for these, so comparing `cat`
        // against them could never match. Confirmed against a real full-year fetch:
        // minor fasts (Tzom Gedaliah, Asara BeTevet, Taanit Esther, 17 Tammuz, Taanit
        // Bechorot) come back as category="holiday", subcat="fast"; special Shabbatot
        // (Shekalim/Zachor/Parah/HaChodesh/HaGadol/etc.) come back as subcat="shabbat",
        // not the "ss" this code was checking for. Both isFast and isSpecialShabbat
        // were therefore always false on real data until this fix.
        const char* subcat = item["subcat"] | "";
        if (item["yomtov"].is<bool>() && item["yomtov"].as<bool>()) entry->isYomTov = true;
        // category "holiday" only: Hebcal's "Fast begins" item (category
        // "zmanim") also carries subcat "fast", and for Tisha B'Av it sits on
        // the eve - which is not itself a fast day.
        if (strcmp(cat, "holiday") == 0 && strcmp(subcat, "fast") == 0) entry->isFast = true;
        if (strcmp(cat, "roshchodesh") == 0) entry->isRoshChodesh = true;
        if (strcmp(subcat, "shabbat") == 0) entry->isSpecialShabbat = true;
        
        // Which holidays are shown (decided 2026-10-18, deliberately not a
        // setting): none of Hebcal's "minor" ones (Tu BiShvat, Lag BaOmer,
        // Chag HaBanot, Pesach Sheni, ...), and of the "modern" Israeli ones
        // only Yom HaAtzma'ut. Matched on title_orig, which stays English.
        const char* orig = item["title_orig"] | item["title"] | "";
        bool hidden = (strcmp(cat, "holiday") == 0) &&
                      (strcmp(subcat, "minor") == 0 ||
                       (strcmp(subcat, "modern") == 0 && strncmp(orig, "Yom HaAtzma", 11) != 0));

        // Chanukah is worked out from the Hebrew date instead (MessageAggregator,
        // chanukahText()): Hebcal dates its items by the evening of the
        // lighting, a day off from the Hebrew day they belong to.
        if (strncmp(orig, "Chanukah", 8) == 0) hidden = true;

        // Display Strings (Holidays/Rosh Chodesh)
        if (!hidden && (strcmp(cat, "holiday") == 0 || strcmp(cat, "roshchodesh") == 0)) {
            // Every holiday of the day is kept (each gets its own slide), up
            // to two; a Yom Tov goes first.
            String heb = gematrizeTrailingYear(stripNikud(item["hebrew"] | "").c_str(), entry->hYear);
            bool yomTov = item["yomtov"].is<bool>() && item["yomtov"].as<bool>();
            if (!entry->holidayHebrew[0]) {
                StringTools::utf8Copy(entry->holidayHebrew, heb.c_str());
            } else if (yomTov) {
                StringTools::utf8Copy(entry->holidayHebrew2, entry->holidayHebrew);
                StringTools::utf8Copy(entry->holidayHebrew, heb.c_str());
            } else if (!entry->holidayHebrew2[0]) {
                StringTools::utf8Copy(entry->holidayHebrew2, heb.c_str());
            } else {
                LOG_AT(LogLevel::WARN, "Calendar", "A third holiday on %s not shown: %s", dateKey, heb.c_str());
            }
            if (Logging::enabled(LogLevel::DEBUG)) CLOCK_LOG("Calendar", "[DataTrace] Parser -> Holiday [%s]: %s", dateKey, heb.c_str());
        }
    }
    delete doc;
}

// Hands a copy of the calendar to the background task, which writes the
// file (writeCacheFile()) - the screen doesn't wait for it. A newer copy
// replaces one not written yet.
void Calendar::saveCache() {
    ensureTask();
    delete s_saveSnapshot.exchange(new std::vector<DayInfo>(s_manifest));
    xTaskNotifyGive(s_fetchTaskHandle);
}

// Checksum (FNV-1a) of the cache file's content as writeCacheFile() would
// write it, 0 = unknown. A download that brings nothing new (every boot,
// mostly) isn't written again: the write is ~30 KB of flash, and while flash
// is written the screen pauses too, whichever task writes (CLAUDE.md,
// "Writing flash pauses everything"). s_cacheFileHash: top of this file.
struct HashWriter {
    uint32_t h = 2166136261u;
    size_t write(uint8_t c) { h = (h ^ c) * 16777619u; return 1; }
    size_t write(const uint8_t* s, size_t n) { for (size_t i = 0; i < n; i++) write(s[i]); return n; }
};
static void buildCacheDoc(const std::vector<DayInfo>& days, JsonDocument& doc);
static uint32_t cacheHash(const std::vector<DayInfo>& days) {
    JsonDocument doc;
    buildCacheDoc(days, doc);
    HashWriter w;
    serializeJson(doc, w);
    return w.h | 1;   // never 0 (= unknown)
}

// Background task only. Into a temp file, then renamed: a restart mid-write
// leaves the previous file, never half of one.
static void writeCacheFile(const std::vector<DayInfo>& days) {
    JsonDocument doc;
    buildCacheDoc(days, doc);
    HashWriter w;
    serializeJson(doc, w);
    uint32_t hash = w.h | 1;
    if (hash == s_cacheFileHash) {
        CLOCK_LOG("Calendar", "[DataTrace] Cache unchanged - not written");
        return;
    }
    if (!FileTools::writeJsonAtomic(CACHE_PATH, doc)) return;   // whole or not at all (a cut-off file passed before)
    s_cacheFileHash = hash;
    CLOCK_LOG("Calendar", "[DataTrace] Cache SAVED: %d entries in manifest", (int)days.size());
}

static void buildCacheDoc(const std::vector<DayInfo>& days, JsonDocument& doc) {
    JsonArray arr = doc.to<JsonArray>();
    arr.add<JsonObject>()["v"] = CACHE_FORMAT;   // first element: format marker, see loadCache()
    for (const auto& d : days) {
        JsonObject obj = arr.add<JsonObject>();
        obj["g"] = d.gregorianDate;
        obj["h"] = d.hebrewDateStr;
        obj["ss"] = d.sunset;
        obj["tz"] = d.tzeit;
        obj["fb"] = d.fastBegin;
        obj["fe"] = d.fastEnd;
        obj["fn"] = d.fastName;
        obj["cl"] = d.candleLightStr;
        obj["hv"] = d.havdalahStr;
        obj["om"] = d.omerHebrew;
        obj["od"] = d.omerDay;
        obj["pa"] = d.parashaHebrew;
        obj["ho"] = d.holidayHebrew;
        obj["h2"] = d.holidayHebrew2;
        obj["mv"] = d.mevarchimHebrew;
        obj["df"] = d.dafYomi;
        obj["my"] = d.mishnaYomit;
        obj["li"] = d.liturgy;
        obj["yt"] = d.isYomTov;
        obj["if"] = d.isFast;
        obj["rc"] = d.isRoshChodesh;
        obj["sh"] = d.isSpecialShabbat;
        obj["hy"] = d.hYear;
        obj["hm"] = d.hMonth;
        obj["hd"] = d.hDay;
        obj["is"] = d.isShabbat;
    }
}

bool Calendar::loadCache() {
    if (!LittleFS.exists(CACHE_PATH)) return false;
    File f = LittleFS.open(CACHE_PATH, "r");
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    // A file from an older firmware - different fields, or holidays filed
    // under rules since changed - is dropped, and the calendar downloaded anew.
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0 || (arr[0]["v"] | 0) != CACHE_FORMAT) {
        CLOCK_LOGF("Calendar", Texts::cacheOldFormat);
        return false;
    }
    s_manifest.clear();
    for (JsonObject obj : arr) {
        if (obj["v"].is<int>()) continue;   // the format marker
        // A file is never assumed to hold only what our code wrote (CLAUDE.md):
        // a day that isn't a real date is left out, a time that isn't one empty.
        int gy, gm, gd;
        if (!DateRules::parseIsoDate(obj["g"] | "", gy, gm, gd)) continue;
        s_manifest.emplace_back();
        DayInfo& d = s_manifest.back();
        memset(&d, 0, sizeof(DayInfo));
        snprintf(d.gregorianDate, sizeof(d.gregorianDate), "%04d-%02d-%02d", gy, gm, gd);
        auto time_ = [](char* dst, size_t n, const char* src) {
            int h, mi, s;
            if (DateRules::parseTime(src, h, mi, s)) StringTools::utf8Cut(dst, n, src, n - 1); else dst[0] = 0;
        };
        // Texts as the parser left them (nikud, "פרשת ", spelling): an older
        // firmware's file was turned away above by its format.
        StringTools::utf8Copy(d.hebrewDateStr, obj["h"] | "");
        time_(d.sunset, sizeof(d.sunset), obj["ss"] | "");
        time_(d.tzeit, sizeof(d.tzeit), obj["tz"] | "");
        time_(d.fastBegin, sizeof(d.fastBegin), obj["fb"] | "");
        time_(d.fastEnd, sizeof(d.fastEnd), obj["fe"] | "");
        StringTools::utf8Copy(d.fastName,  obj["fn"] | "");
        time_(d.candleLightStr, sizeof(d.candleLightStr), obj["cl"] | "");
        time_(d.havdalahStr, sizeof(d.havdalahStr), obj["hv"] | "");
        StringTools::utf8Copy(d.omerHebrew, obj["om"] | "");
        d.omerDay = obj["od"] | 0;
        StringTools::utf8Copy(d.parashaHebrew, obj["pa"] | "");
        StringTools::utf8Copy(d.holidayHebrew, obj["ho"] | "");
        StringTools::utf8Copy(d.holidayHebrew2, obj["h2"] | "");
        StringTools::utf8Copy(d.mevarchimHebrew, obj["mv"] | "");
        StringTools::utf8Copy(d.dafYomi, obj["df"] | "");
        StringTools::utf8Copy(d.mishnaYomit, obj["my"] | "");
        StringTools::utf8Copy(d.liturgy, obj["li"] | "");
        d.isYomTov = obj["yt"] | false;
        d.isFast = obj["if"] | false;
        d.isRoshChodesh = obj["rc"] | false;
        d.isSpecialShabbat = obj["sh"] | false;
        d.hYear  = obj["hy"] | 0;
        d.hMonth = obj["hm"] | 0;
        d.hDay   = obj["hd"] | 0;
        if (!DateRules::validHebrew(d.hYear, d.hMonth, d.hDay) || d.hYear < 1) {   // not one: from the Gregorian day
            HebrewDate h = JewishDateEngine::calculateHebrew({gy, gm, gd, false});
            d.hYear = h.year; d.hMonth = h.month; d.hDay = h.day;
        }
        d.isShabbat = obj["is"] | false;
    }
    // What's on file, as writeCacheFile() would write it now (dates that weren't
    // real dates left out, above): an unchanged download then isn't written again.
    s_cacheFileHash = cacheHash(s_manifest);
    CLOCK_LOG("Calendar", "[DataTrace] Cache LOADED: %d entries in manifest", s_manifest.size());
    return true;
}

// Asked by the web server, which runs alongside the main loop (on the other
// core since 2026-10-22): the main loop itself empties the calendar, in
// update(), never while it's reading it.
void Calendar::clearCacheAndRefresh() { s_clearCacheRequested = true; }

void Calendar::forceRefresh() { s_forceRefresh = true; }

// The debug time just jumped (WebUI, /api/debug/time): check at once whether
// the new date is in the saved calendar and download it if not - instead of
// after the spacing between downloads. Unlike forceRefresh(), a date that is
// already there is not downloaded again (that rebuild hid candles/parasha
// for ~40 s after a jump, 2026-10-21).
void Calendar::timeJumped() { s_timeJumped = true; }

// Shared lookup for getUpcomingParasha()/getUpcomingMevarchim(): finds the
// nearest Shabbat from todayDate onward (today itself counts, UNLESS it's a
// Shabbat whose own Havdalah has already passed - see below), via the
// isShabbat flag each manifest entry already carries, and returns THAT one
// day's field - so the upcoming Shabbat's Parasha/Mevarchim shows all week,
// not just on the day itself. Deliberately NOT "scan forward for the first
// non-empty field": Mevarchim is empty most weeks, and scanning past an
// empty-but-real Shabbat to find a later one that happens to be Mevarchim
// would announce it a whole cycle early, on the wrong week entirely. Pinning
// to the one actual nearest Shabbat and returning whatever (or nothing) is
// on it avoids that - Parasha is unaffected either way, since it's populated
// on every Shabbat and that's always the nearest one found here too. 8 days
// is a full week of margin to find A Shabbat at all; the manifest is always
// kept at least CALENDAR_DAYS_AHEAD_MIN + CALENDAR_DAYS_MARGIN days ahead of today (see the fetch logic above), so this
// never runs dry.
static String findOnNearestShabbat(const std::vector<DayInfo>& manifest, const char* todayDate, char (DayInfo::*field)[80]) {
    int startIdx = -1;
    for (size_t i = 0; i < manifest.size(); i++) {
        if (strcmp(manifest[i].gregorianDate, todayDate) == 0) { startIdx = (int)i; break; }
    }
    if (startIdx == -1) return String();

    // Today's own civil-day window (used by Candle/Havdalah) runs until
    // midnight, but Parasha/Mevarchim should flip the moment Shabbat itself
    // actually ends - at Havdalah - not linger showing "this week" for
    // several more hours after it's over. Checked here against the real
    // clock (independent of cfg.rolloverMode, which governs a different,
    // user-configurable concept - the Hebrew-date rollover - not this one
    // specific, non-configurable transition) rather than civil midnight.
    if (manifest[startIdx].isShabbat && manifest[startIdx].tzeit[0]) {
        time_t nowLocal = NTPManager::getLocalEpoch();
        struct tm tmb;
        gmtime_r(&nowLocal, &tmb);
        char nowTime[9];
        strftime(nowTime, sizeof(nowTime), "%H:%M:%S", &tmb);
        if (strcmp(nowTime, manifest[startIdx].tzeit) >= 0) {
            startIdx++;
        }
    }

    for (int i = startIdx; i < (int)manifest.size() && i < startIdx + 8; i++) {
        if (!manifest[i].isShabbat) continue;
        const char* val = manifest[i].*field;
        return val[0] ? String(val) : String();
    }
    return String();
}

const DayInfo& Calendar::getToday() { return s_today; }
const DayInfo& Calendar::getCivilToday() { return s_civilToday; }
String Calendar::getUpcomingParasha()   { return findOnNearestShabbat(s_manifest, s_civilToday.gregorianDate, &DayInfo::parashaHebrew); }
String Calendar::getUpcomingMevarchim() { return findOnNearestShabbat(s_manifest, s_civilToday.gregorianDate, &DayInfo::mevarchimHebrew); }

const char* Calendar::getTodaySummary() { return s_today.hebrewDateStr; }

// Fast-times slide, decided per CIVIL day (midnight to midnight) so it reads
// the same all day: shown from midnight the day before a fast until midnight
// after it ends - including after the end time, for anyone a little late.
// Hebcal's begin time tells the two fast shapes apart: a morning begin is a
// dawn-to-night fast (begin and end on the same day); an evening begin is a
// full-day fast that started the evening before (Tisha B'Av), so its begin
// sits on the eve and its end on the next day. Yom Kippur has no Hebcal fast
// events at all - its eve gets the seudah mafseket notice (until candle
// lighting, when Hebcal starts the fast); its end is the existing Havdalah slide.
// The fast a calendar day starts, in the shapes Hebcal gives (2026-11-14: was
// written out in both getFastNotice()s): a dawn-to-night fast (begin and end on
// that day), an evening-start fast (Tisha B'Av: begins that evening, ends the
// next day), or the eve of Yom Kippur (seudah mafseket until candle lighting).
// t1 = the next day, nullptr if the calendar ends. Days counted from t0.
namespace {
struct FastStart { FastNotice n; int endDay = 0, fastDay = 0; };
bool isMorning(const char* hhmm) { return atoi(hhmm) < 12; }
bool fastStartingOn(const DayInfo& t0, const DayInfo* t1, FastStart& f) {
    f = FastStart{};
    FastNotice& n = f.n;
    if (t0.fastBegin[0] && isMorning(t0.fastBegin)) {          // dawn to night
        n.kind = FastNotice::Kind::Fast;
        StringTools::utf8Copy(n.begin, t0.fastBegin);
        StringTools::utf8Copy(n.end, t0.fastEnd);
        StringTools::utf8Copy(n.name, t0.fastName);
    } else if (t0.fastBegin[0]) {                              // evening start
        n.kind = FastNotice::Kind::Fast;
        StringTools::utf8Copy(n.begin, t0.fastBegin);
        if (t1) StringTools::utf8Copy(n.end, t1->fastEnd);
        StringTools::utf8Copy(n.name, t1 && t1->fastName[0] ? t1->fastName : t0.fastName);
        f.endDay = f.fastDay = 1;
    } else if (t0.candleLightStr[0] && t1 && strncmp(t1->holidayHebrew, "יום כיפור", strlen("יום כיפור")) == 0) {
        n.kind = FastNotice::Kind::SeudaMafseket;
        StringTools::utf8Copy(n.end, t0.candleLightStr);
        f.fastDay = 1;
    } else {
        return false;
    }
    return true;
}
}  // namespace

FastNotice Calendar::getFastNotice() {
    FastNotice n;
    int i = -1;
    for (size_t k = 0; k < s_manifest.size(); k++) {
        if (strcmp(s_manifest[k].gregorianDate, s_civilToday.gregorianDate) == 0) { i = (int)k; break; }
    }
    if (i < 0) return n;
    const DayInfo& t0 = s_manifest[i];
    const DayInfo* t1 = i + 1 < (int)s_manifest.size() ? &s_manifest[i + 1] : nullptr;
    FastStart f;
    // Today's own fast (or the eve of an evening-start one / of Yom Kippur)
    // first; then the 2nd day of an evening-start fast; then the day before a
    // dawn-start one. (The eve of Yom Kippur and the day before a dawn fast
    // can't meet: Yom Kippur itself starts no fast.)
    if (fastStartingOn(t0, t1, f) && f.n.kind == FastNotice::Kind::Fast) {
        n = f.n;
        n.endInDays = f.endDay;
    } else if (t0.fastEnd[0]) {                                // 2nd day of an evening-start fast
        n.kind = FastNotice::Kind::Fast;
        StringTools::utf8Copy(n.end, t0.fastEnd);
        StringTools::utf8Copy(n.name, t0.fastName);
    } else if (t1 && t1->fastBegin[0] && isMorning(t1->fastBegin) && fastStartingOn(*t1, nullptr, f)) {   // day before a dawn-start fast
        n = f.n;
        n.beginInDays = 1;
        n.endInDays = 1;
    } else if (fastStartingOn(t0, t1, f)) {                    // the eve of Yom Kippur
        n = f.n;
    }
    return n;
}

// ── Show windows (the user's father, 2026-11-08) ─────────────────────────────
// When candle lighting, Havdalah, the fast times, the parasha and Shabbat
// Mevarchim show: AppConfig's ShowSpan per slide (Config.h). Moments are
// local seconds (days since 1970 * 86400 + time of day), the same scale as
// NTPManager::getLocalEpoch(). The default windows keep the code above
// (getCivilToday(), findOnNearestShabbat(), getFastNotice()) - nothing about
// them changed.

static long dayNumberOf(const DayInfo& d) {
    int y = 0, m = 0, dd = 0;
    if (!DateRules::parseIsoDate(d.gregorianDate, y, m, dd)) return LONG_MIN;
    return (long)daysFromCivil(y, m, dd);
}

// "HH:MM" or "HH:MM:SS" on day `day`, or -1 when there is no time.
static int64_t momentAt(long day, const char* hhmm) {
    int h = 0, mi = 0, s = 0;
    if (!DateRules::parseTime(hhmm, h, mi, s)) return -1;
    return (int64_t)day * 86400 + h * 3600 + mi * 60 + s;
}

static int64_t midnightStarting(long day) { return (int64_t)day * 86400; }

static void civilFromDays(long z, int& y, int& m, int& d) {   // H. Hinnant's civil_from_days
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400 + (m <= 2));
}

using Manifest = std::vector<DayInfo>;

static const DayInfo* entryForDay(const Manifest& man, long day) {
    for (const DayInfo& e : man) if (dayNumberOf(e) == day) return &e;
    return nullptr;
}

// When the Jewish day that follows civil day `day` begins: that evening's
// sunset or צאת הכוכבים (the Hebrew-date change setting), or midnight when
// that setting is midnight. A day without the time in the calendar gets it
// computed, as syncToday() does.
static int64_t eveningOf(const Manifest& man, long day, const AppConfig& cfg) {
    if (cfg.rolloverMode == RolloverMode::Midnight) return midnightStarting(day + 1);
    const bool tzeit = cfg.rolloverMode == RolloverMode::Tzeit;
    const DayInfo* e = entryForDay(man, day);
    const char* t = e ? (tzeit ? e->tzeit : e->sunset) : "";
    char calc[10] = "";
    if (!t[0]) {
        int y, m, d;
        civilFromDays(day, y, m, d);
        computeZman(y, m, d, cfg.latitude, cfg.longitude, tzeit, calc, sizeof(calc));
        t = calc;
    }
    const int64_t at = momentAt(day, t);
    return at >= 0 ? at : midnightStarting(day + 1);
}

static int64_t nowLocalSeconds() { return (int64_t)NTPManager::getLocalEpoch(); }
static long dayOfMoment(int64_t t) { return (long)(t >= 0 ? t / 86400 : (t - 86399) / 86400); }

// Candle lighting / Havdalah. Of the times whose window holds now, the next
// one still ahead; if all have passed, the latest (shown on until its window
// ends). Two can overlap only around a two-day Yom Tov or one next to Shabbat.
static TimedNotice timedNotice(const Manifest& man, char (DayInfo::*field)[10], const ShowSpan& w, const AppConfig& cfg) {
    TimedNotice out;
    const int64_t now = nowLocalSeconds();
    const long today = dayOfMoment(now);
    int64_t bestAt = 0;
    bool bestAhead = false, found = false;
    for (const DayInfo& e : man) {
        const char* time = e.*field;
        if (!time[0]) continue;
        const long day = dayNumberOf(e);
        const int64_t at = momentAt(day, time);
        if (at < 0) continue;
        const int64_t from = w.from == ShowFrom::Evening ? eveningOf(man, day - 1, cfg) : midnightStarting(day);
        const int64_t until = w.until == ShowUntil::AtTime ? at : midnightStarting(day + 1);
        if (now < from || now >= until) continue;
        const bool ahead = at > now;
        const bool better = !found || (ahead && (!bestAhead || at < bestAt)) || (!ahead && !bestAhead && at > bestAt);
        if (!better) continue;
        found = true; bestAhead = ahead; bestAt = at;
        StringTools::utf8Copy(out.time, time);
        out.inDays = (int)(day - today);
    }
    return out;
}

TimedNotice Calendar::getCandle(const AppConfig& cfg) {
    const ShowSpan& w = cfg.candleShow;
    if (w.from == ShowFrom::Midnight && w.until == ShowUntil::Midnight) {
        TimedNotice out;
        StringTools::utf8Copy(out.time, s_civilToday.candleLightStr);
        return out;
    }
    return timedNotice(s_manifest, &DayInfo::candleLightStr, w, cfg);
}

TimedNotice Calendar::getHavdalah(const AppConfig& cfg) {
    const ShowSpan& w = cfg.havdalahShow;
    if (w.from == ShowFrom::Midnight && w.until == ShowUntil::Midnight) {
        TimedNotice out;
        StringTools::utf8Copy(out.time, s_civilToday.havdalahStr);
        return out;
    }
    return timedNotice(s_manifest, &DayInfo::havdalahStr, w, cfg);
}

// When Shabbat `e` ends: its Havdalah; a Shabbat followed by Yom Tov has a
// candle lighting instead; else צאת הכוכבים; else midnight.
static int64_t shabbatEnd(const DayInfo& e, long day) {
    for (const char* t : { e.havdalahStr, e.candleLightStr, e.tzeit }) {
        const int64_t at = momentAt(day, t);
        if (at >= 0) return at;
    }
    return midnightStarting(day + 1);
}

// Parasha / Mevarchim with a window other than "all week": from Friday
// (its midnight, or the Jewish Friday's start - Thursday evening) until the
// end of Shabbat or the midnight after it.
static String shabbatText(const Manifest& man, char (DayInfo::*field)[80], const ShowSpan& w, const AppConfig& cfg) {
    const int64_t now = nowLocalSeconds();
    for (const DayInfo& e : man) {
        if (!e.isShabbat || !(e.*field)[0]) continue;
        const long day = dayNumberOf(e);
        const int64_t from = w.from == ShowFrom::Evening ? eveningOf(man, day - 2, cfg) : midnightStarting(day - 1);
        const int64_t until = w.until == ShowUntil::AtTime ? shabbatEnd(e, day) : midnightStarting(day + 1);
        if (now >= from && now < until) return String(e.*field);
    }
    return String();
}

String Calendar::getParasha(const AppConfig& cfg) {
    if (cfg.parashaShow.from == ShowFrom::AllWeek) return getUpcomingParasha();
    return shabbatText(s_manifest, &DayInfo::parashaHebrew, cfg.parashaShow, cfg);
}

String Calendar::getMevarchim(const AppConfig& cfg) {
    const ShowSpan& w = cfg.mevarchimShow.from == ShowFrom::LikeParasha ? cfg.parashaShow : cfg.mevarchimShow;
    if (w.from == ShowFrom::AllWeek || w.from == ShowFrom::LikeParasha) return getUpcomingMevarchim();
    return shabbatText(s_manifest, &DayInfo::mevarchimHebrew, w, cfg);
}

// The fast slide by the user's window. Each fast in the calendar, in the
// three shapes getFastNotice() knows: a dawn-to-night fast (begin and end on
// its day), an evening-start fast (Tisha B'Av: begins on the eve, ends the
// next day), and the eve of Yom Kippur (seudah mafseket until candle
// lighting). "Day before" = from midnight of the day before the fast's day
// (what getFastNotice() does); "Evening before" = from the evening before
// the fast begins - the start of the Jewish day it begins in. The words
// "מחר" / "מחרתיים" count civil days from today, as people say them.
FastNotice Calendar::getFastNotice(const AppConfig& cfg) {
    const ShowSpan& w = cfg.fastShow;
    if (w.from == ShowFrom::Midnight && w.until == ShowUntil::Midnight) return getFastNotice();
    const int64_t now = nowLocalSeconds();
    const long today = dayOfMoment(now);
    for (size_t k = 0; k < s_manifest.size(); k++) {
        const DayInfo& t0 = s_manifest[k];
        const DayInfo* t1 = k + 1 < s_manifest.size() ? &s_manifest[k + 1] : nullptr;
        const long day = dayNumberOf(t0);
        FastNotice n;
        long beginDay = day, endDay = day, fastDay = day;
        FastStart f;
        // The order of the code before fastStartingOn() (checked case by case,
        // sim/test_fast_merge.py): a fast beginning that day, then the end of one
        // begun before the calendar's first day, then the eve of Yom Kippur.
        auto take = [&]() { n = f.n; endDay = day + f.endDay; fastDay = day + f.fastDay; };
        if (t0.fastBegin[0] && fastStartingOn(t0, t1, f)) {               // dawn start, evening start
            take();
        } else if (k == 0 && t0.fastEnd[0]) {                              // its eve is before the calendar's first day
            n.kind = FastNotice::Kind::Fast;
            StringTools::utf8Copy(n.end, t0.fastEnd);
            StringTools::utf8Copy(n.name, t0.fastName);
            beginDay = day - 1;
        } else if (fastStartingOn(t0, t1, f)) {                            // the eve of Yom Kippur
            take();
        } else {
            continue;
        }
        const int64_t from = w.from == ShowFrom::Evening ? eveningOf(s_manifest, beginDay - 1, cfg) : midnightStarting(fastDay - 1);
        const int64_t endAt = momentAt(endDay, n.end);
        const int64_t until = (w.until == ShowUntil::AtTime && endAt >= 0) ? endAt : midnightStarting(endDay + 1);
        if (now < from || now >= until) continue;
        if (beginDay < today) n.begin[0] = 0;   // started on an earlier day: "עד 19:57"
        n.beginInDays = (int)(beginDay - today);
        n.endInDays = (int)(endDay - today);
        return n;
    }
    return FastNotice();
}

// Both rain/dew notices count civil days (midnight to midnight), not the
// clock's Hebrew day - "ממחר" all through the eve until midnight, then
// "ממוסף" (the user, 2026-11-02). Every switch lies well inside the Hebrew
// year of today's civil date (Tishrei 22, Cheshvan / early December, Nisan),
// so a +-2 day window never crosses Rosh Hashana.
static int civilHebrewYear(long civilAbs) {
    int y, m, d;
    JewishDateEngine::absoluteToHebrew(civilAbs, y, m, d);
    return y;
}

// The mention, switching at Musaf of the switch day. Pesach's is shown only
// from its eve on (the user, 2026-11-01: nobody needs reminding that Pesach
// is coming, and before it the screen is full enough).
// Ashkenazim outside Israel say nothing (בלי משיב הרוח) in summer instead of
// מוריד הטל, so a stop notice shows for them (the request's ותן ברכה still shows).
PrayerSwitchNotice Calendar::getPrayerSwitch(int windowDays, Nusach nusach, bool inIsrael, long civilTodayAbs) {
    PrayerSwitchNotice n;
    if (civilTodayAbs <= 0) return n;   // date not known yet
    if (windowDays < 0) windowDays = 0;
    if (windowDays > 2) windowDays = 2;
    const int hy = civilHebrewYear(civilTodayAbs);
    static const struct { int month, day; const char* phrase; } kSwitches[] = {
        { 1, 22, "משיב הרוח" },   // Tishrei 22, Shemini Atzeret
        { 7, 15, "מוריד הטל" },   // Nisan 15, first day of Pesach
    };
    for (const auto& sw : kSwitches) {
        bool ashkenazAbroad = sw.month == 7 && !inIsrael && nusach == Nusach::Ashkenaz;
        const char* phrase = ashkenazAbroad ? "בלי משיב הרוח" : sw.phrase;
        long d = civilTodayAbs - JewishDateEngine::hebrewToAbsolute(hy, sw.month, sw.day);
        const int before = sw.month == 7 ? (windowDays > 0 ? 1 : 0) : windowDays;   // Pesach: only its eve
        if (d >= -before && d <= windowDays) {
            n.phrase = phrase;
            n.daysFromSwitch = (int)d;
            return n;
        }
    }
    return n;
}

// The request switches at the first weekday Maariv - ברכת השנים isn't said
// on Shabbat or Yom Tov. Its "day" is the civil day whose evening that is:
// winter - the eve of 7 Cheshvan in Israel, early December abroad; summer -
// the last day of Pesach's first Yom Tov (Israel 15 Nisan, abroad 16 Nisan),
// never before (the user, 2026-11-02: no ותן ברכה on Yom Tov, and on its
// eve only מוריד הטל). An evening that starts Shabbat moves to Motzei
// Shabbat (abroad: 4/5 December on a Friday; 16 Nisan on a Friday).
RainRequestNotice Calendar::getRainRequest(int windowDays, Nusach nusach, bool inIsrael, long civilTodayAbs) {
    RainRequestNotice n;
    if (civilTodayAbs <= 0) return n;
    if (windowDays < 0) windowDays = 0;
    if (windowDays > 2) windowDays = 2;
    const bool sefard = nusach == Nusach::Sefard;
    const int hy = civilHebrewYear(civilTodayAbs);
    auto notFriday = [](long abs) { return abs % 7 == 5 ? abs + 1 : abs; };   // abs % 7: 6 = Shabbat

    long winterDay;   // the civil day whose evening starts the winter request
    if (inIsrael) {
        winterDay = JewishDateEngine::hebrewToAbsolute(hy, HebrewMonths::CHESHVAN, 7) - 1;
    } else {
        // 60 days after the autumn tekufah of the Talmud's (Julian) calendar:
        // Maariv of 21 November Julian, 22 November before a Julian leap year.
        // In Gregorian dates: 4/5 December now, 13 days added; the gap grows
        // by a day each century not divisible by 400 (2100: 5/6 December).
        int gy, gm, gd;
        JewishDateEngine::absoluteToGregorian(JewishDateEngine::hebrewToAbsolute(hy, HebrewMonths::TISHREI, 1), gy, gm, gd);
        int julianGap = gy / 100 - gy / 400 - 2;
        int day = 21 + ((gy + 1) % 4 == 0 ? 1 : 0) + julianGap - 30;   // November 21+gap = December (21+gap-30)
        winterDay = JewishDateEngine::gregorianToAbsolute(gy, 12, day);
    }
    winterDay = notFriday(winterDay);
    long d = civilTodayAbs - winterDay;
    if (d >= -windowDays && d <= windowDays) {   // this evening is always shown (windowDays 0 still says when)
        n.phrase = sefard ? "ברך עלינו" : "ותן טל ומטר";
        n.daysFromSwitch = (int)d;
        return n;
    }
    long summerDay = notFriday(JewishDateEngine::hebrewToAbsolute(hy, HebrewMonths::NISSAN, inIsrael ? 15 : 16));
    d = civilTodayAbs - summerDay;
    if (d >= 0 && d <= windowDays) {
        n.phrase = sefard ? "ברכנו" : "ותן ברכה";
        n.daysFromSwitch = (int)d;
    }
    return n;
}

String Calendar::formatUserHebrewDate(const char* fullDate) {
    if (!fullDate || !*fullDate) return "";
    String s = String(fullDate);
    s.replace(" \xD7\x94\xD7\xB3", " "); // Strip Millennium 5
    return s;
}
