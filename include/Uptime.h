#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Uptime: the ONLY clock for "how long since" in this program (2026-11-14).
//
// The clock runs for months or years without a restart. Arduino's millis() is
// 32-bit and starts over every 49.7 days; a comparison written the wrong way
// round, or a special value like "0 = now", then goes wrong after 24.8 or
// 49.7 days (it did: NTP stopped syncing for weeks, NTPManager.cpp). So:
//
//   - Uptime::ms() / us(): time since start, 64-bit, from the ESP32's own
//     64-bit microsecond counter (esp_timer). Starts over after 292,000 years.
//   - Timer: "has DURATION passed since start()?". Not running is its own
//     state - never a special time value.
//
// millis() and micros() are not used anywhere else in src/ or include/ -
// scripts/check_rules.py fails the build if they come back.
// On the simulator esp_timer_get_time() is the same 64-bit clock
// (sim/stubs/esp_timer.h; JC_UPTIME_START_MS starts it late, for tests).
// ─────────────────────────────────────────────────────────────────────────────
#include <stdint.h>
#include <esp_timer.h>

namespace Uptime {
    inline int64_t us() { return esp_timer_get_time(); }
    inline int64_t ms() { return esp_timer_get_time() / 1000; }
}

// A stopwatch on Uptime::ms(). Not thread-safe: one task uses it (a value
// shared between tasks is an std::atomic<int64_t> of Uptime::ms() instead).
class Timer {
public:
    void    start()                        { _startMs = Uptime::ms(); _running = true; }
    void    startAt(int64_t nowMs)         { _startMs = nowMs; _running = true; }
    void    stop()                         { _running = false; }
    bool    running() const                { return _running; }
    // Time since start(); 0 when not running.
    int64_t elapsedMs() const              { return _running ? Uptime::ms() - _startMs : 0; }
    int64_t elapsedMs(int64_t nowMs) const { return _running ? nowMs - _startMs : 0; }
    // Running and at least durationMs since start(). Not running = false.
    bool    passed(int64_t durationMs) const              { return _running && Uptime::ms() - _startMs >= durationMs; }
    bool    passed(int64_t durationMs, int64_t nowMs) const { return _running && nowMs - _startMs >= durationMs; }
    // Not running, or durationMs passed: "may I do it again?" (a pause after the last time).
    bool    idleOrPassed(int64_t durationMs) const        { return !_running || passed(durationMs); }
private:
    int64_t _startMs = 0;
    bool    _running = false;
};
