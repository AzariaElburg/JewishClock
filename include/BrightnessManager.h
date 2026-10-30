#pragma once

// =============================================================================
// BrightnessManager.h
// JewishClock - Backlight brightness: manual, or following the room's light.
//
// Levels are perceived brightness in % (0 = backlight off, 100 = full), turned
// into PWM with CIE lightness, so equal steps look equal. With the light
// sensor on, the level follows the room light (in %, a log scale - lightPct())
// between its dark and light edges, from the faintest (nightPct) to the
// brightest (dayPct) level. The edges can learn the room by themselves
// (learnRoomLight). Details: BrightnessManager.cpp.
// =============================================================================

#include "Config.h"

class BrightnessManager {
public:
    // Call once in setup(); initialises the light sensor. Returns whether it
    // was found (see AmbientLight::begin()).
    static bool begin(const AppConfig& cfg);

    // Call every loop() iteration: reads the sensor, learns the room's edges
    // (may change and save cfg), fades toward the target level, writes the
    // PWM when it changes.
    static void loop(AppConfig& cfg);

    // Lux -> room light in % (0..100, log scale).
    static float lightPct(float lux);

    // Where the level is heading for this room light (sensor mode), in %.
    static float autoLevelPct(float roomPct, const AppConfig& cfg);

    // Show this level at once, for a few seconds (BRIGHTNESS_PREVIEW_MS) -
    // while the brightness slider moves. Any task.
    static void preview(float pct);

    // The user set the room's edges: learning starts a new window. Any task.
    static void edgesChangedByUser();

    // What's on the backlight now: perceived % and the PWM duty (0..BACKLIGHT_DUTY_MAX).
    static float currentPct();
    static uint32_t currentDuty();

    // Perceived % -> PWM duty (CIE 1931 lightness).
    static uint32_t dutyForPct(float pct);
};
