#pragma once

// =============================================================================
// NTPManager.h
// JewishClock - Raw UDP NTP client with step/slew/ignore time discipline.
//
// TIME CORRECTION POLICY (all thresholds tunable at top of .cpp):
//
//   offset ≥ 60 s   → Hard step: jump immediately.  Used on first sync and
//                      when clock has drifted badly (power-cycle, long outage).
//
//   300 ms – 60 s   → Slew: gradually correct at up to 5 ms per real second.
//                      No visible jump; the clock smoothly converges.
//
//   < 300 ms        → Ignore: within acceptable jitter for a scrolling display.
//                      Repeated 5ms adjustments are unnecessary for this use.
//
// WHY NOT USE THE ARDUINO NTPClient LIBRARY?
//   It calls settimeofday() which causes visible time jumps and has no slew
//   support.  This implementation models time in software and only applies
//   corrections through the slew mechanism, giving smooth continuous time.
//
// USAGE:
//   NTPManager::begin(servers, count, tzPOSIX);
//   // in main loop:
//   NTPManager::loop();
//   // to get current time:
//   time_t t = NTPManager::getTime();
// =============================================================================

#include <Arduino.h>
#include <string.h>
#include <time.h>
#include "Config.h"   // NTP_SERVER_SLOTS, NTP_SERVER_MAX_CHARS

namespace NTPManager {

    struct DateTime {
        int year;   // e.g. 2026
        int month;  // 1-12
        int day;    // 1-31
        int hour;
        int minute;
        int second;
        bool isDST;
    };

    // Call once in setup(). servers: the settings' list (NTP_SERVER_SLOTS, Config.h;
    // empty entries skipped). The clock uses one at a time and moves on to the next
    // when it doesn't answer (no address for its name, or no reply) - 2026-11-13.
    void    begin(const char* const* servers, int count, const char* tzPOSIX = nullptr);

    // The list changed (settings): from the web task too - takes NTPLock. Keeps the
    // server in use if it's still in the list.
    void    setServers(const char* const* servers, int count);

    // A server name the clock can use: a host name or IPv4 address, 1..NTP_SERVER_MAX_CHARS
    // letters, digits, '.', '-' (no '.'/'-' at either end, no ".."). Says nothing about
    // whether it answers (testStart()). Inline: Storage uses it in both simulators too.
    inline bool serverNameValid(const char* name) {
        size_t n = name ? strlen(name) : 0;
        if (n == 0 || n > (size_t)NTP_SERVER_MAX_CHARS) return false;
        if (name[0] == '.' || name[0] == '-' || name[n - 1] == '.' || name[n - 1] == '-') return false;
        for (size_t i = 0; i < n; i++) {
            char c = name[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            if (!ok || (c == '.' && name[i + 1] == '.')) return false;
        }
        return true;
    }

    // Ask one server once, apart from the clock's own syncing (the page checks a server
    // before saving it). Runs in loop(); testResult() polls. A new start replaces a test
    // still running.
    void    testStart(const char* server);
    // state: 0 none, 1 running, 2 answered, 3 failed; detail: why it failed, or the
    // server's time against the clock's in ms when it answered.
    void    testResult(int& state, String& server, String& detail);

    // The server in use now (for /api/status).
    String  currentServer();

    // Call from the main loop (non-blocking; polls at configured intervals).
    void    loop();

    // Updates the local timezone string (POSIX format)
    void    setTimezone(const char* tzPOSIX);

    // Sets the country code for pool.ntp.org substitution (called by Location::activate).
    // cc = ISO 3166-1 alpha-2, upper or lower case ("IL", "il"); "" = clear (use global pool).
    void    setCountryPool(const char* cc);

    // Sets a manual offset for debugging (time travel)
    void    setDebugOffset(int64_t offset);

    // Returns UTC Unix timestamp. Returns 0 if not yet synced.
    time_t  getTime();
    // One moment per main-loop pass (2026-11-21, the user: "midnight is not to break anything - either
    // ignored or taken into account, not left to chance"): main.cpp's loop() calls beginPass() first and
    // endPass() last; in between every time read (getTime / getDateTime / getLocalEpoch - also from the
    // web task meanwhile) is that one moment. A new day, hour or minute can then only come BETWEEN passes:
    // the calendar's "today" and the screen's can never be two different days.
    void    beginPass();
    void    endPass();

    // Fills a DateTime struct with smoothed local time (respecting DST/TZ).
    // Returns false if not yet synced.
    bool    getDateTime(DateTime& out);

    // Helper to get raw local epoch (UTC + dynamic offset)
    time_t  getLocalEpoch();

    // Trigger an immediate sync attempt on the next loop() call.
    void    forceSync();
    int64_t utcMs();      // UTC in ms by the clock's model, no debug offset; 0 = not synced (tests)
    int64_t lastRoundTripMs();   // the last used reply's round trip, ms as measured; -1 = none yet (its time is right within half)

    // True if at least one successful NTP sync has occurred.
    bool    isSynced();

    // Milliseconds since last successful sync (0 if never synced).
    int64_t msSinceSync();

    // From the main loop: keeps TIME_SEEN_PATH (the latest real time seen,
    // for reading NTP's 32-bit seconds right past 2162) - a short write every
    // 30 days, on a still screen.
    void    serviceTimeSeen();

    // A server "not recommended" (NTPManager.cpp troubleWith(): it failed while the internet was
    // there) - asked last, once a day; cleared after needDays good days. Kept in NTP_TROUBLE_PATH,
    // written from the main loop by serviceTroubleFile() on a still screen.
    struct TroubleInfo { String server; String why; int64_t sinceUnix; int goodDays; int needDays; bool refused; };
    int     troubleList(TroubleInfo* out, int max);
    bool    hasRefusedServer();
    void    serviceTroubleFile();

} // namespace NTPManager
