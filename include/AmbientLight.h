#pragma once

// =============================================================================
// AmbientLight.h
// JewishClock - BH1750 ambient light sensor interface.
//
// The BH1750 (I2C) is wired in and read for real when begin() detects it.
// If detection fails - this has been seen intermittently, most likely a
// genuine I2C timeout/NACK rather than a software bug - readLux() falls
// back to a simulated indoor value (SIMULATED_LUX) instead of failing outright.
// Lux -> brightness lives in BrightnessManager.cpp.
//
// Usage:
//   AmbientLight::begin();          // call in setup()
//   AmbientLight::loop();           // call every iteration of main loop
// =============================================================================

#include <Arduino.h>
#include "Storage.h"

namespace AmbientLight {
    // Call once in setup() — initialises I2C and the sensor. Returns whether
    // the sensor was actually found (false = running on the simulated value).
    bool begin();

    // Call every loop() iteration — samples lux and updates moving average
    void loop();

    // Current smoothed lux value (moving average - SMOOTHING_ALPHA in AmbientLight.cpp)
    float readLux();

    // Returns true if a real BH1750 sensor is connected.
    bool isSensorAvailable();
}
