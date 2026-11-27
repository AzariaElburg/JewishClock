#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OnlineUpdate: new versions from the internet (2026-11-14, item 5 of the agreed
// order). The clock asks a release feed - the GitHub releases of the public
// JewishClock-Updates repository (Config.h UPDATE_FEED_URL; a setting may name
// another, for tests) - once a day; a newer version is installed at 03:00 when
// "Automatic updates" is on, or at once with "Install now" in Settings. The
// package goes through the same Updater as a file from the page: signature
// checked (UpdateSignature.h), firmware on trial, back by itself if it fails.
//
// The network part runs on the one background task (Calendar.cpp's download
// task - runInTask()); the main loop only decides when (loop()).
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "Config.h"

namespace OnlineUpdate {
    void loop(const AppConfig& cfg);   // main loop: when to check, when to install
    void checkNow();                   // Settings: "Check now"
    void useFeed(const char* feed);    // the feed setting changed: in use from the next check ("" = UPDATE_FEED_URL)
    // The feed found not there (its repository gone - the user, 2026-11-15): the local day (since
    // 1970) first found so, 0 = it is there. Said in the log and Settings at once; on the clock's
    // screen ("Error - see Settings") once it has lasted FEED_MISSING_SCREEN_AFTER_DAYS.
    long feedMissingSince();
    bool feedMissingOnScreen();
    bool feedMissingSeen();      // Settings' "OK, seen" given: not on the screen (Settings still lists it)
    void feedMissingSeenNow();
    void installNow();                 // Settings: "Install now" (a newer version known)
    void runInTask();                  // the download task: the waiting check / download

    struct Status {
        String state;       // "idle" | "checking" | "downloading" | "installing" | "restarting"
        String available;   // a newer version on the feed, "" = none known
        String lastCheck;   // "YYYY-MM-DD HH:MM" (local) of the last answer, "" = never
        String message;     // the last check's or install's outcome, for people
    };
    Status status();

    // "2.3.0" newer than "2.2.1-4-g1a2b3c4"? (a.b.c, then commits after the tag)
    bool newer(const String& candidate, const String& running);
}
