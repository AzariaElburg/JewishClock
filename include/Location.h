#pragma once
#include <Arduino.h>
#include "Config.h"

// Where the clock is: the place of the Wi-Fi network it's on (2026-10-24).
// Each known network has its own Place (AppConfig::places, Config.h). Joining
// a network makes its place the active one - the AppConfig location fields
// the calendar downloads use, and the time-zone rule the clock's time uses.
// A network without a place gets one lookup (ip-api, via Calendar's
// background fetch); the user can accept it or pick another (search, page).
//
// All changes to the configuration happen in loop(), on the main loop task;
// the web server's task only queues them (queuePlace(), Calendar requests).
namespace Location {

    // Main loop: applies a queued choice from the page, and notices the clock
    // joining a network (WiFiManager::connectedSlot()).
    void loop(AppConfig& cfg);

    // A lookup's answer (ip-api JSON) for network `slot` - main loop only
    // (Calendar's fetch result handling). False = unusable answer.
    bool applyLookup(AppConfig& cfg, int slot, const String& payload);

    // A place chosen in the page for network `slot` (any task): checked,
    // completed (rule from the table), applied by loop() shortly after.
    bool queuePlace(int slot, const Place& p, String& error);

    // Network `slot` was replaced or removed: its place goes too (any task;
    // done by loop()). A new network there gets its own lookup.
    void forgetPlace(int slot);

    // The POSIX rule for an IANA zone name, from FS_TZ_TABLE; "" if not in it.
    // Reads a small file - fine on any task, not per frame.
    String posixFor(const char* tzid);

    // True for a place in Israel (Israeli calendar, the built-in time rule).
    bool isIsrael(const Place& p);

    // ── Place search (correcting a place in the page) ──
    // The clock searches, not the browser: at the parents' the laptop is on
    // the clock's own Wi-Fi, without internet. Free geocoding service
    // (open-meteo), via Calendar's background fetch.
    void   requestSearch(const String& query);    // web task
    String searchStatus();                        // web task: {"state":"idle|waiting|done|error", "results":[...]}
    bool   searchToStart(String& query);          // main loop (Calendar::update): a search is waiting
    void   searchStarted();
    void   searchDone(int httpCode, const String& payload);
    String searchUrl(const String& query);
}
