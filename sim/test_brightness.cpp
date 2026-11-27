// Brightness learning (src/BrightnessManager.cpp, unchanged) on simulated
// days: the room's light is made up here, time runs a second per step. The
// narrowing needs 3 days without a restart - too long to watch on the clock.
// Built and run by sim/test_brightness.py; exit code 1 on any wrong result.

#include "BrightnessManager.h"
#include "AmbientLight.h"
#include "Display.h"
#include "Storage.h"
#include "DiagnosticLog.h"
#include "Tunables.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>

// --- stand-ins for what BrightnessManager uses ---
static uint64_t g_ms = 0;   // the clock's uptime (Uptime::ms() - 64-bit since 2026-11-14)
static float g_lux = 1;
static int g_saves = 0;
static uint64_t g_lastSaveMs = 0, g_minSaveGapMs = UINT64_MAX;
static int g_logLines = 0;

unsigned long millis() { return (unsigned long)g_ms; }
int64_t simUptimeUs() { return (int64_t)g_ms * 1000; }   // esp_timer_get_time() (sim/stubs/esp_timer.h)
bool AmbientLight::begin() { return true; }
void AmbientLight::loop() {}
float AmbientLight::readLux() { return g_lux; }
bool AmbientLight::isSensorAvailable() { return true; }
void DisplayManager::setBrightness(uint32_t) {}
void Storage::saveConfigSoon() {
    if (g_saves && g_ms - g_lastSaveMs < g_minSaveGapMs) g_minSaveGapMs = g_ms - g_lastSaveMs;
    g_saves++; g_lastSaveMs = g_ms;
}
void DiagnosticLog::record(const char* cat, const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("      log [%s] ", cat); vprintf(fmt, a); printf("\n");
    va_end(a); g_logLines++;
}

// --- the test ---
static int g_fails = 0;
static void check(bool ok, const char* what, float got, float want) {
    printf("  %-5s %s: %.1f (expected %.1f)\n", ok ? "ok" : "WRONG", what, got, want);
    if (!ok) g_fails++;
}
static void near(const char* what, float got, float want) { check(fabsf(got - want) < 0.6f, what, got, want); }

static AppConfig makeCfg(float dark, float light) {
    static AppConfig c;   // static: zero-initialised
    c.useLightSensor = true; c.learnRoomLight = true;
    c.nightPct = 10; c.dayPct = 90; c.roomDarkPct = dark; c.roomLightPct = light;
    return c;
}

// A day: 8 h at nightLux, 16 h at dayLux, one step per second.
static void runDays(AppConfig& cfg, float days, float nightLux, float dayLux) {
    unsigned long steps = (unsigned long)(days * 86400);
    for (unsigned long i = 0; i < steps; i++) {
        unsigned long secOfDay = i % 86400;
        g_lux = secOfDay < 8 * 3600 ? nightLux : dayLux;
        BrightnessManager::loop(cfg);
        g_ms += 1000;
    }
}

static void start(AppConfig& cfg, uint64_t ms) {
    g_ms = ms; g_saves = 0; g_minSaveGapMs = UINT64_MAX; g_logLines = 0;
    BrightnessManager::begin(cfg);
    BrightnessManager::edgesChangedByUser();   // a new window, as at boot
}

int main() {
    // The clock's own numbers, asked of its register (Tunables.h) - never copied here (the user, 2026-11-19)
    const float W = (float)Tunables::number("LEARN_WINDOW_DAYS");      // days of light before narrowing
    const float GAP = (float)Tunables::number("LEARN_MIN_GAP_PCT");    // the edges at least this far apart
    const double SAVE_MS = Tunables::number("LEARN_SAVE_MIN_MS");      // learned edges saved at most this often
    if (std::isnan(W) || std::isnan(GAP) || std::isnan(SAVE_MS)) { printf("WRONG: a tunable not shared\n"); return 1; }
    const float N = BrightnessManager::lightPct(1);     // 20%: 1 lux at night
    const float D = BrightnessManager::lightPct(300);   // 69.5%: 300 lux by day

    printf("1. Wider at once: edges 30..60, room %.1f..%.1f\n", N, D);
    AppConfig c = makeCfg(30, 60); start(c, 5000);
    runDays(c, 0.01f, 1, 300);   // 14 min of night
    near("dark edge after minutes of night", c.roomDarkPct, N);
    runDays(c, 1, 1, 300);
    near("light edge after a day", c.roomLightPct, D);
    runDays(c, W, 1, 300);
    near("dark edge a window on (room came near: stays)", c.roomDarkPct, N);
    near("light edge a window on", c.roomLightPct, D);
    check(g_logLines == 0, "lines logged (no narrowing)", (float)g_logLines, 0);

    printf("2. Narrower after %.0f days: edges 0..90, room %.1f..%.1f\n", W, N, D);
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, W - 0.1f, 1, 300);
    near("dark edge before the window ends", c.roomDarkPct, 0);
    near("light edge before the window ends", c.roomLightPct, 90);
    runDays(c, 0.2f, 1, 300);
    near("dark edge after it", c.roomDarkPct, N);
    near("light edge after it", c.roomLightPct, D);

    printf("3. Minimum gap: edges 0..90, room always 100 lux (%.1f%%)\n", BrightnessManager::lightPct(100));
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, W + 0.1f, 100, 100);
    near("dark edge", c.roomDarkPct, BrightnessManager::lightPct(100) - GAP / 2);
    near("light edge", c.roomLightPct, BrightnessManager::lightPct(100) + GAP / 2);

    printf("4. A restart (new window) before the window ends: no narrowing\n");
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, W * 2 / 3, 1, 300);
    BrightnessManager::edgesChangedByUser();
    runDays(c, W * 2 / 3, 1, 300);
    near("dark edge", c.roomDarkPct, 0);
    near("light edge", c.roomLightPct, 90);

    printf("5. Up 49.7 days (where 32-bit millis() wrapped) and 1000 years, inside the window\n");
    c = makeCfg(0, 90); start(c, 0xFFFFFFFFULL - 86400000ULL);   // crosses 2^32 ms after 1 day
    runDays(c, W + 0.1f, 1, 300);
    near("dark edge", c.roomDarkPct, N);
    near("light edge", c.roomLightPct, D);
    c = makeCfg(0, 90); start(c, 1000ULL * 365 * 86400 * 1000);   // a thousand years up
    runDays(c, W + 0.1f, 1, 300);
    near("dark edge", c.roomDarkPct, N);
    near("light edge", c.roomLightPct, D);

    printf("6. Saves: at most one every %.0f min\n", SAVE_MS / 60000);
    c = makeCfg(50, 50.5f); start(c, 5000);
    const float gapDays = (float)(SAVE_MS / 86400000.0);
    for (int h = 0; h < 6; h++) runDays(c, gapDays, 1.0f / (h + 1), 1000.0f * (h + 1));   // a new extreme each save gap
    check(g_saves >= 4, "saves in 6 save gaps of new extremes", (float)g_saves, 5);
    check(g_minSaveGapMs >= SAVE_MS, "shortest gap between saves, minutes", g_minSaveGapMs / 60000.0f, (float)(SAVE_MS / 60000));

    printf("brightness learning test: %s\n", g_fails ? "WRONG" : "ALL CORRECT");
    return g_fails ? 1 : 0;
}
