#pragma once
// The full simulator's own pieces (jcclock only) - see sim/README.md.
#include <mutex>
#include <string>
#include <cstdint>
#include "sim_platform.h"

namespace Sim {
    // The clock's main loop and every web request run one at a time under
    // this lock: on the clock they run side by side (core 1 / core 0), and
    // its own locks keep them apart - those really lock here too.
    std::recursive_mutex& clockLock();

    // Ports on the PC for the clock's two network sides (sim_clock_main.cpp
    // options): the home network (STA, the address the clock got there) and
    // its own access point (192.168.4.1).
    extern int homePort, apPort;
    void startWebListeners();          // sim_web.cpp

    // Wi-Fi (sim_wifi.cpp)
    bool staUp();                      // joined a network, with an address
    bool apUp();                       // its own access point is on
    std::string staIpString();
    void wifiPumpEvents();             // driver events to the clock's handler - main loop, before loop()
    bool wifiInternet();               // the joined network reaches the internet
    void loadWorld(const std::string& path);
    std::string worldJson();
    bool setWorldJson(const std::string& json, std::string& err);

    // Where the simulated clock's non-LittleFS state lives: firmware slots,
    // RTC memory, the world (<state>/...). Next to the LittleFS folder.
    extern std::string stateDir;
    extern std::string firmwareVersion;   // the version this program plays (its slot's)
}
