// Brightness learning (src/BrightnessManager.cpp, unchanged) on simulated
// days: the room's light is made up here, time runs a second per step. The
// narrowing needs 3 days without a restart - too long to watch on the clock.
// Built and run by sim/test_brightness.py; exit code 1 on any wrong result.

#include "BrightnessManager.h"
#include "AmbientLight.h"
#include "Display.h"
#include "Storage.h"
#include "DiagnosticLog.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>

// --- stand-ins for what BrightnessManager uses ---
static unsigned long g_ms = 0;
static float g_lux = 1;
static int g_saves = 0;
static unsigned long g_lastSaveMs = 0, g_minSaveGapMs = 0xFFFFFFFF;
static int g_logLines = 0;

unsigned long millis() { return g_ms; }
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

static void start(AppConfig& cfg, unsigned long ms) {
    g_ms = ms; g_saves = 0; g_minSaveGapMs = 0xFFFFFFFF; g_logLines = 0;
    BrightnessManager::begin(cfg);
    BrightnessManager::edgesChangedByUser();   // a new window, as at boot
}

int main() {
    const float N = BrightnessManager::lightPct(1);     // 20%: 1 lux at night
    const float D = BrightnessManager::lightPct(300);   // 69.5%: 300 lux by day

    printf("1. Wider at once: edges 30..60, room %.1f..%.1f\n", N, D);
    AppConfig c = makeCfg(30, 60); start(c, 5000);
    runDays(c, 0.01f, 1, 300);   // 14 min of night
    near("dark edge after minutes of night", c.roomDarkPct, N);
    runDays(c, 1, 1, 300);
    near("light edge after a day", c.roomLightPct, D);
    runDays(c, 3, 1, 300);
    near("dark edge 3 days on (room came near: stays)", c.roomDarkPct, N);
    near("light edge 3 days on", c.roomLightPct, D);
    check(g_logLines == 0, "lines logged (no narrowing)", (float)g_logLines, 0);

    printf("2. Narrower after 3 days: edges 0..90, room %.1f..%.1f\n", N, D);
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, 2.9f, 1, 300);
    near("dark edge before 3 days", c.roomDarkPct, 0);
    near("light edge before 3 days", c.roomLightPct, 90);
    runDays(c, 0.2f, 1, 300);
    near("dark edge after 3 days", c.roomDarkPct, N);
    near("light edge after 3 days", c.roomLightPct, D);

    printf("3. Minimum gap: edges 0..90, room always 100 lux (%.1f%%)\n", BrightnessManager::lightPct(100));
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, 3.1f, 100, 100);
    near("dark edge", c.roomDarkPct, BrightnessManager::lightPct(100) - 10);
    near("light edge", c.roomLightPct, BrightnessManager::lightPct(100) + 10);

    printf("4. A restart (new window) before 3 days: no narrowing\n");
    c = makeCfg(0, 90); start(c, 5000);
    runDays(c, 2, 1, 300);
    BrightnessManager::edgesChangedByUser();
    runDays(c, 2, 1, 300);
    near("dark edge", c.roomDarkPct, 0);
    near("light edge", c.roomLightPct, 90);

    printf("5. millis() wrapping (every 49.7 days) inside the window\n");
    c = makeCfg(0, 90); start(c, 0xFFFFFFFFUL - 86400000UL);   // wraps after 1 day
    runDays(c, 3.1f, 1, 300);
    near("dark edge", c.roomDarkPct, N);
    near("light edge", c.roomLightPct, D);

    printf("6. Saves: at most one an hour\n");
    c = makeCfg(50, 50.5f); start(c, 5000);
    for (int h = 0; h < 6; h++) runDays(c, 1.0f / 24, 1.0f / (h + 1), 1000.0f * (h + 1));   // a new extreme each hour
    check(g_saves >= 4, "saves in 6 hours of new extremes", (float)g_saves, 5);
    check(g_minSaveGapMs >= 3600000UL, "shortest gap between saves, minutes", g_minSaveGapMs / 60000.0f, 60);

    printf("brightness learning test: %s\n", g_fails ? "WRONG" : "ALL CORRECT");
    return g_fails ? 1 : 0;
}
