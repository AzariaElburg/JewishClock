#include "AmbientLight.h"
#include "Config.h"
#include <math.h>
#include <Wire.h>
#include "StringTools.h"
#include "DiagnosticLog.h"
#include <BH1750.h>

namespace AmbientLight {

// === TUNABLES ===  (rule: see Config.h header)
static const float         SIMULATED_LUX      = 500.0f; // Stand-in when the sensor isn't found: indoor daylight
static const unsigned long SAMPLE_INTERVAL_MS = 500;
// EMA weight of a new reading; higher = more responsive. 0.3 at 2 readings/s
// follows a light switched on within ~1.5 s; the brightness fades on top
// (BrightnessManager.cpp), which smooths flicker and passing shadows.
static const float         SMOOTHING_ALPHA    = 0.3f;
// Most sensitive setting (2026-10-29): high-res mode 2 at the longest
// measuring time (~0.12 lux steps, ~0.45 s per reading, up to ~8900 lux) -
// behind the box's pinhole a dark room reads near 0, and "nearly dark" must
// still be told apart from "dark". Daylight behind the pinhole stays far
// below the top of the range.
static const uint8_t       SENSOR_MTREG       = 254;

static BH1750 g_lightMeter(BH1750_ADDR);
static bool g_sensorFound = false;
static float g_lastLux = SIMULATED_LUX;
static unsigned long g_lastSampleMs = 0;

bool begin() {
    Wire.begin(BH1750_SDA, BH1750_SCL);
    g_sensorFound = g_lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE_2);
    if (g_sensorFound) g_lightMeter.setMTreg(SENSOR_MTREG);
    if (g_sensorFound) {
        CLOCK_LOG("AmbientLight", "BH1750 sensor found and initialized.");
    } else {
        // Persisted: detection fails intermittently, and this is the record
        // of when. A reset doesn't clear it - only a full power-off does (see
        // CLAUDE.md, "A reset doesn't power-cycle anything wired to the board").
        DiagnosticLog::record("SENSOR", "BH1750 not found (I2C 0x%02X, SDA=GPIO%d SCL=GPIO%d) - using simulated %.0f lux",
                              BH1750_ADDR, BH1750_SDA, BH1750_SCL, SIMULATED_LUX);
    }
    return g_sensorFound;
}

bool isSensorAvailable() {
    return g_sensorFound;
}

float readLux() {
    return g_lastLux;
}

void loop() {
    unsigned long now = millis();
    if (now - g_lastSampleMs < SAMPLE_INTERVAL_MS) return;
    g_lastSampleMs = now;

    if (!g_sensorFound) {
        // In simulation mode, we don't need to do anything.
        // g_lastLux remains at its default.
        return;
    }

    if (g_lightMeter.measurementReady()) {
        float rawLux = g_lightMeter.readLightLevel();
        if (rawLux < 0) {
            // On a read error, hold the last good value
            return;
        }

        // Apply smoothing (Exponential Moving Average)
        g_lastLux = SMOOTHING_ALPHA * rawLux + (1.0f - SMOOTHING_ALPHA) * g_lastLux;
    }
}

}; // namespace AmbientLight
