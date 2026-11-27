#pragma once
// The full simulator's own pieces (jcclock only) - see sim/README.md.
#include <mutex>
#include <atomic>
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
    std::string apSsid(), apPass();    // its name and password (sim/hotspot.py makes it a real Wi-Fi)
    std::string staIpString();
    void wifiPumpEvents();             // driver events to the clock's handler - main loop, before loop()
    bool wifiInternet();               // the joined network reaches the internet
    bool serviceBlocked(const std::string& what);   // the world's "blocked": "ntp", or a download's URL
    bool fakeAnswer(const std::string& url, std::string& body);   // fake keywords: sets body for the fake answer
    int  fakeStatus(const std::string& url);                      // "retry429:": 429, else 200
    bool fakeHdr(const std::string& url, const std::string& name, std::string& value);  // "ratelimit:"/"retry429:": fake headers
    void loadWorld(const std::string& path);
    std::string worldJson();
    bool setWorldJson(const std::string& json, std::string& err);
    // A time server on this PC for the tests (sim/ntp_drill.py): packets to 127.x.x.x port 123
    // go to this port instead (0 = none; real time servers are never on 127.x). Control /ntp-port.
    extern std::atomic<int> localNtpPort;

    // Where the simulated clock's non-LittleFS state lives: firmware slots,
    // RTC memory, the world (<state>/...). Next to the LittleFS folder.
    extern std::string stateDir;
    extern std::string firmwareVersion;   // the version this program plays (its slot's)
}
