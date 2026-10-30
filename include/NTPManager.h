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
//   NTPManager::begin("pool.ntp.org", tzOffsetSeconds);
//   // in main loop:
//   NTPManager::loop();
//   // to get current time:
//   time_t t = NTPManager::getTime();
// =============================================================================

#include <Arduino.h>
#include <time.h>

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

    // Call once in setup().  tzOffsetSec is added to UTC when returning local time.
    void    begin(const char* server = "pool.ntp.org", const char* tzPOSIX = nullptr);

    // Call from the main loop (non-blocking; polls at configured intervals).
    void    loop();

    // Updates the local timezone string (POSIX format)
    void    setTimezone(const char* tzPOSIX);

    // Sets a manual offset for debugging (time travel)
    void    setDebugOffset(int64_t offset);

    // Returns UTC Unix timestamp. Returns 0 if not yet synced.
    time_t  getTime();

    // Fills a DateTime struct with smoothed local time (respecting DST/TZ).
    // Returns false if not yet synced.
    bool    getDateTime(DateTime& out);

    // Helper to get raw local epoch (UTC + dynamic offset)
    time_t  getLocalEpoch();

    // Trigger an immediate sync attempt on the next loop() call.
    void    forceSync();

    // True if at least one successful NTP sync has occurred.
    bool    isSynced();

    // Milliseconds since last successful sync (0 if never synced).
    unsigned long millisSinceSync();

} // namespace NTPManager
