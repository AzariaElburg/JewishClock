// =============================================================================
// BrightnessManager.cpp
// JewishClock - Backlight brightness: manual, or following the room's light.
// =============================================================================
//
// The model (2026-10-29), the one phones and Windows use for automatic
// brightness, as one smooth curve:
//  1. The light reading (smoothed in AmbientLight) becomes "room light" in %
//     on a LOG scale (lightPct()) - the eye judges light by ratios, so
//     1 -> 10 lux feels like 100 -> 1000.
//  2. Its place between the room's dark and light edges (roomDarkPct /
//     roomLightPct, set by the user and/or learned) picks a perceived level
//     between the faintest and brightest brightness (nightPct / dayPct).
//  3. Perceived % -> PWM with CIE 1931 lightness (the standard curve for
//     dimming LEDs): equal steps look equal, and the dark end gets the fine
//     steps it needs (12-bit PWM, Config.h BACKLIGHT_PWM_BITS).
//  4. The backlight fades to it: fast when the room gets brighter, slower
//     when it gets darker - never a jump.
//
// Learning (learnRoomLight): the edges follow the room.
//  - Wider at once: a minute's average darker than the dark edge (or lighter
//    than the light edge) becomes the new edge. Minute averages, so a torch
//    or a reflection for a few seconds doesn't count.
//  - Narrower slowly: the darkest and lightest minute of each LEARN_WINDOW_DAYS
//    are kept as a second pair of marks; at the window's end an edge the room
//    didn't come near (LEARN_NEAR_PCT) moves to its mark - a moved clock, a
//    new lamp, the seasons. Never closer than LEARN_MIN_GAP_PCT apart.
//  The marks live in memory only: after a restart a window starts over.
//  Learned edges are saved at most once an hour, when the screen stands
//  still (Storage::saveConfigSoon()) - a flash write pauses the screen
//  (CLAUDE.md "Writing flash pauses everything"), unseen on a still frame.

#include "BrightnessManager.h"
#include "AmbientLight.h"
#include "Display.h"
#include "Storage.h"
#include "DiagnosticLog.h"
#include "Uptime.h"
#include "Tunables.h"
#include <math.h>
#include <atomic>

// === TUNABLES ===  (rule: see Config.h header)
// Room light scale: 0% = this many lux or darker, +20% per x10 (100% = 10000
// lux; the sensor's top, ~7400 lux in its sensitive mode, is ~97%).
static const float LIGHT_PCT_ZERO_LUX   = 0.1f;
static const float LIGHT_PCT_PER_DECADE = 20.0f;
// Fade time constants: the level covers ~63% of the way in this time.
static const float FADE_UP_SEC   = 0.7f;   // the room got brighter (a light came on)
static const float FADE_DOWN_SEC = 3.0f;   // the room got darker
// How long a brightness slider move shows its level on the clock.
static const uint32_t BRIGHTNESS_PREVIEW_MS = 4000;
// Learning (see above).
static const int64_t  LEARN_AVG_MS       = 60000;    // only light lasting this long counts
static const int64_t  LEARN_WINDOW_DAYS  = 3;        // an edge not come near this long moves in
TUNABLE_SHARED(LEARN_WINDOW_DAYS)
static const float    LEARN_NEAR_PCT     = 5.0f;     // "near" an edge, in room-light %
static const float    LEARN_MIN_GAP_PCT  = 20.0f;    // edges never closer (x10 in lux)
TUNABLE_SHARED(LEARN_MIN_GAP_PCT)
static const int64_t  LEARN_SAVE_MIN_MS  = 3600000;  // learned edges saved at most this often
TUNABLE_SHARED(LEARN_SAVE_MIN_MS)

static float    s_levelPct = -1;          // current level (perceived %), -1 = not set yet
static uint32_t s_duty = 0;               // what's on the PWM now
static Timer    s_sinceLastLoop;          // dt between loops (not running = first loop)
static std::atomic<int>      s_previewTenths{-1};   // preview level x10, -1 = none
static std::atomic<int64_t>  s_previewUntilMs{0};   // Uptime::ms(); only read while s_previewTenths >= 0

// Learning state (main loop only, except the reset flag).
static std::atomic<bool> s_learnReset{true};   // start a new window (boot, user edit)
static double   s_avgSum = 0;
static uint32_t s_avgN = 0;
static int64_t  s_avgStartMs = 0;
static float    s_winMin = 0, s_winMax = 0;     // the window's darkest/lightest minute
static bool     s_winAny = false;
static int64_t  s_winStartMs = 0;
static bool     s_dirty = false;
static int64_t  s_lastSaveMs = 0;

bool BrightnessManager::begin(const AppConfig& cfg) {
    s_lastSaveMs = Uptime::ms();
    return AmbientLight::begin();
}

float BrightnessManager::lightPct(float lux) {
    if (lux <= LIGHT_PCT_ZERO_LUX) return 0;
    float p = LIGHT_PCT_PER_DECADE * log10f(lux / LIGHT_PCT_ZERO_LUX);
    return p > 100 ? 100 : p;
}

uint32_t BrightnessManager::dutyForPct(float pct) {
    if (pct <= 0) return 0;
    if (pct > 100) pct = 100;
    // CIE 1931: lightness L* (0..100) -> relative luminance Y (0..1).
    float y = (pct <= 8.0f) ? pct / 903.3f : powf((pct + 16.0f) / 116.0f, 3.0f);
    uint32_t d = (uint32_t)lroundf(y * BACKLIGHT_DUTY_MAX);
    return d < 1 ? 1 : d;   // > 0% always lights it, however faintly
}

float BrightnessManager::autoLevelPct(float roomPct, const AppConfig& cfg) {
    float span = cfg.roomLightPct - cfg.roomDarkPct;
    if (span < 1) span = 1;
    float pos = (roomPct - cfg.roomDarkPct) / span;
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    return cfg.nightPct + (cfg.dayPct - cfg.nightPct) * pos;
}

void BrightnessManager::preview(float pct) {
    s_previewTenths = (int)lroundf(pct * 10);
    s_previewUntilMs = Uptime::ms() + BRIGHTNESS_PREVIEW_MS;
}

void BrightnessManager::edgesChangedByUser() { s_learnReset = true; }

float BrightnessManager::currentPct() { return s_levelPct < 0 ? 0 : s_levelPct; }
uint32_t BrightnessManager::currentDuty() { return s_duty; }

// One minute's average room light: widen the edges at once, track the window.
static void learnMinute(AppConfig& cfg, float avg, int64_t now) {
    if (avg < cfg.roomDarkPct)  { cfg.roomDarkPct  = avg; s_dirty = true; }
    if (avg > cfg.roomLightPct) { cfg.roomLightPct = avg; s_dirty = true; }
    if (!s_winAny || avg < s_winMin) s_winMin = avg;
    if (!s_winAny || avg > s_winMax) s_winMax = avg;
    s_winAny = true;

    if (now - s_winStartMs < LEARN_WINDOW_DAYS * 86400000LL) return;
    // Window over: move in an edge the room didn't come near, keeping the gap.
    float dark = cfg.roomDarkPct, light = cfg.roomLightPct;
    if (s_winMin > dark + LEARN_NEAR_PCT)   dark = s_winMin;
    if (s_winMax < light - LEARN_NEAR_PCT)  light = s_winMax;
    if (light - dark < LEARN_MIN_GAP_PCT) {
        // Too close: the minimum gap around their middle, slid to stay inside
        // the old edges (old edges already closer, set by hand: leave them).
        if (cfg.roomLightPct - cfg.roomDarkPct <= LEARN_MIN_GAP_PCT) {
            dark = cfg.roomDarkPct; light = cfg.roomLightPct;
        } else {
            float mid = (dark + light) / 2;
            dark = mid - LEARN_MIN_GAP_PCT / 2;
            if (dark < cfg.roomDarkPct) dark = cfg.roomDarkPct;
            if (dark + LEARN_MIN_GAP_PCT > cfg.roomLightPct) dark = cfg.roomLightPct - LEARN_MIN_GAP_PCT;
            light = dark + LEARN_MIN_GAP_PCT;
        }
    }
    if (dark != cfg.roomDarkPct || light != cfg.roomLightPct) {
        DiagnosticLog::record("LIGHT", "learned room edges moved in: dark %.1f -> %.1f%%, light %.1f -> %.1f%% (%u days: %.1f..%.1f%%)",
                              cfg.roomDarkPct, dark, cfg.roomLightPct, light,
                              (unsigned)LEARN_WINDOW_DAYS, s_winMin, s_winMax);
        cfg.roomDarkPct = dark;
        cfg.roomLightPct = light;
        s_dirty = true;
    }
    s_winAny = false;
    s_winStartMs = now;
}

static void learn(AppConfig& cfg, int64_t now) {
    if (s_learnReset.exchange(false) || !cfg.learnRoomLight || !AmbientLight::isSensorAvailable()) {
        s_avgSum = 0; s_avgN = 0; s_avgStartMs = now;
        s_winAny = false; s_winStartMs = now;
        if (!cfg.learnRoomLight) return;
    }
    if (!AmbientLight::isSensorAvailable()) return;
    s_avgSum += BrightnessManager::lightPct(AmbientLight::readLux());
    s_avgN++;
    if (now - s_avgStartMs >= LEARN_AVG_MS) {
        learnMinute(cfg, (float)(s_avgSum / s_avgN), now);
        s_avgSum = 0; s_avgN = 0; s_avgStartMs = now;
    }
    int64_t sinceSave = now - s_lastSaveMs;
    if (s_dirty && sinceSave >= LEARN_SAVE_MIN_MS) {
        Storage::saveConfigSoon();
        s_dirty = false;
        s_lastSaveMs = now;
    }
}

void BrightnessManager::loop(AppConfig& cfg) {
    AmbientLight::loop();
    int64_t now = Uptime::ms();
    float dt = s_sinceLastLoop.elapsedMs(now) / 1000.0f;   // 0 the first time
    s_sinceLastLoop.startAt(now);

    learn(cfg, now);

    float target = cfg.useLightSensor ? autoLevelPct(lightPct(AmbientLight::readLux()), cfg) : cfg.brightPct;
    int prev = s_previewTenths;
    if (prev >= 0) {
        if (now < s_previewUntilMs) { s_levelPct = prev / 10.0f; target = s_levelPct; }
        else s_previewTenths = -1;   // over: fade back from the previewed level
    }

    if (s_levelPct < 0 || !cfg.useLightSensor) {
        s_levelPct = target;   // first time, and manual changes: at once
    } else if (dt > 0) {
        float tau = (target > s_levelPct) ? FADE_UP_SEC : FADE_DOWN_SEC;
        s_levelPct += (target - s_levelPct) * (1.0f - expf(-dt / tau));
        if (fabsf(target - s_levelPct) < 0.05f) s_levelPct = target;
    }

    // Starts at 0 = what DisplayManager::begin() left on the PWM.
    uint32_t duty = dutyForPct(s_levelPct);
    if (duty != s_duty) {
        DisplayManager::setBrightness(duty);
        s_duty = duty;
    }
}
