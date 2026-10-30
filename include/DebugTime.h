#pragma once

#include <atomic>
#include <cstdint>
#include <time.h>

// The debug time offset (POST /api/debug/time), readable WITHOUT NTPLock so
// log timestamps can show the time the firmware is acting on - real or
// simulated. Defined and written in NTPManager.cpp. Log code must not call
// NTPManager's locked getters: NTPManager logs while holding NTPLock, and a
// timestamp lookup through it deadlocked the main loop (2026-10-16).
extern std::atomic<int64_t> g_debugTimeOffsetSec;   // 64-bit: test dates beyond 68 years from now

// Set by NTPManager on its first sync this boot. Checked instead of "is the
// system clock past 1970": a software restart (OTA update, restore) keeps
// the ESP32's clock running, so right after one the clock already holds a
// plausible time - but in UTC, before the timezone is applied, and before
// the firmware itself trusts it. Logs showed it as local time, 3 h off
// (found 2026-10-16).
extern std::atomic<bool> g_timeSyncedThisBoot;

// System time plus the debug offset, or 0 before the first sync this boot.
// NTPManager sets the system clock once, at its first sync; later
// corrections go into its own time model only. Both run off the same
// crystal, so they stay within a fraction of a second of each other -
// close enough for a log timestamp. Lock-free.
inline time_t debugAdjustedNow() {
    if (!g_timeSyncedThisBoot.load()) return 0;
    return time(nullptr) + g_debugTimeOffsetSec.load();
}
