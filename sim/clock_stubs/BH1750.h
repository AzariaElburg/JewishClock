#pragma once
// PC stand-in (jcclock): the light sensor reads the simulator's room light
// (Sim::roomLux, set on the control port - sim/README.md). Lux as the real
// sensor reports it at the clock's settings (AmbientLight.cpp scales MTreg).
#include <Arduino.h>
#include "sim_platform.h"
class BH1750 {
public:
    enum Mode { CONTINUOUS_HIGH_RES_MODE = 0x10, CONTINUOUS_HIGH_RES_MODE_2 = 0x11, ONE_TIME_HIGH_RES_MODE = 0x20 };
    explicit BH1750(uint8_t = 0x23) {}
    bool begin(Mode = CONTINUOUS_HIGH_RES_MODE, uint8_t = 0x23, void* = nullptr) { return Sim::lightSensorPresent; }
    bool setMTreg(uint8_t m) { mt_ = m; return true; }
    bool measurementReady(bool = false) { return true; }
    float readLightLevel() { return Sim::roomLux; }
private:
    uint8_t mt_ = 69;
};
