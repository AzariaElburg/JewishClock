#pragma once
// Simulator controls (simulator only - see sim/README.md).
#include <string>
#include <ctime>

namespace Sim {
    extern bool quiet;              // true = the clock's own log lines are not printed
    extern std::string fsRoot;      // PC folder playing the clock's file system
    extern std::string httpCacheDir;
    void advanceMillis(unsigned long ms);
    void waitForTasks();            // let a running download finish (in zero simulated time)
    void setTimezone(const char* posix);
    long utcOffsetAt(time_t utc);   // seconds east of UTC, DST included
    void setUtc(time_t utc);        // the simulated "now" (sim_modules.cpp)
    time_t utcNow();
    // Startup and download timing in simulated ms: random within ranges
    // measured on the clock (see sim_platform.cpp). startTiming() draws this
    // run's Wi-Fi-up and time-sync moments; each download draws its own.
    struct Range { unsigned long lo, hi; };
    extern Range setupTime, wifiUp, ntpAfterWifi, netMain, netZmanim, netGeo;
    extern unsigned long setupMs, wifiUpMs, ntpSyncMs;
    void startTiming(uint32_t seed);
    void pinTiming(const std::string& name, unsigned long ms);   // "main#2" (2nd main download) or "main" (all)
    unsigned long drawTiming(const Range& r, const std::string& kind);   // one more value, by name (printed)
    // The full simulator (jcclock) only - the screen one leaves them alone:
    extern bool (*internetUp)();    // downloads fail when this says no (no network, or one without internet)
    extern bool liveNetwork;        // downloads for real, uncached, at their real speed (not the measured simulated one)
    extern bool fsFull;             // file writes fail when LittleFS's 4 MB is full (counted as LittleFS counts)
    extern float roomLux;           // what the light sensor reads
    extern bool lightSensorPresent; // false: the clock runs on its simulated value, as without a sensor
}
