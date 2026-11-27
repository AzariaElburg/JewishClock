// =============================================================================
// sim_ntp.cpp - the full simulator's system clock (jcclock). The clock's own
// NTPManager.cpp runs unchanged (real time servers, over sim_net.cpp's UDP and
// name lookup); this is only what ESP-IDF gives it underneath: a system clock
// that counts from 1970 at every start until settimeofday() sets it, then runs
// on with millis() (which follows the PC's tick, as the ESP32's follows its
// crystal).
// =============================================================================

#include <Arduino.h>
#include <sys/time.h>
#include <atomic>
#include "sim_clock.h"
#include "sim_platform.h"

static std::atomic<int64_t> g_msAtZero{0};   // system time (ms since 1970) at the simulator's start (Sim::uptimeMs() 0) - 64-bit, as the ESP32's system time runs on its 64-bit timer, not on millis()

int sim_settimeofday(const struct sim_timeval* tv, const void*) {
    if (tv) g_msAtZero = tv->tv_sec * 1000LL + tv->tv_usec / 1000 - (int64_t)Sim::uptimeMs();
    return 0;
}
int sim_gettimeofday(struct sim_timeval* tv, void*) {
    int64_t ms = g_msAtZero.load() + (int64_t)Sim::uptimeMs();
    if (tv) { tv->tv_sec = ms / 1000; tv->tv_usec = (long)(ms % 1000) * 1000; }
    return 0;
}
time_t sim_time(time_t* out) {
    time_t t = (time_t)((g_msAtZero.load() + (int64_t)Sim::uptimeMs()) / 1000);
    if (out) *out = t;
    return t;
}
// The screen simulator's names for the same clock (sim_platform.h).
void Sim::setUtc(time_t utc) { g_msAtZero = (int64_t)utc * 1000LL - (int64_t)Sim::uptimeMs(); }
time_t Sim::utcNow() { return sim_time(nullptr); }
