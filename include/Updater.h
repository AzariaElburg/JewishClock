#pragma once

#include <Arduino.h>

// =============================================================================
// Updater.h
// JewishClock - installing updates over Wi-Fi, safely, with nobody technical
// on site (the clock lives at the author's parents; updates arrive by email
// and are installed from the settings page's Firmware Update page).
//
// What can be uploaded to POST /update:
//   - a signed .jcupdate package (utils/make_update_package.py): firmware + all
//     the web files from Data/ (never config.json / events.json / the log).
//     Nothing else: a plain firmware .bin, an unsigned or wrongly signed package,
//     or any package while the clock has no update key, is refused (UpdateKeyState.h).
//
// Safety net:
//   - New firmware boots "on trial" (ESP-IDF app rollback). It confirms
//     itself only after joining Wi-Fi and staying up TRIAL_STABLE_MS. If it
//     crashes, hangs, or gets no Wi-Fi within TRIAL_CONNECT_DEADLINE_MS, the
//     clock goes back to the previous firmware by itself.
//   - A package's web files are set aside (/upd) and put in place only once
//     the firmware they belong to has passed its trial - so firmware and web
//     page can never end up from different versions. After a rollback the
//     waiting files are discarded. Put in place without a restart (2026-11-10;
//     until then the clock restarted once more): the web server's file list
//     (WebUI::webFilesChanged()) and the icons in the screen's memory are
//     refreshed, and page files asked for during the move get a short
//     "reload in a moment" (movingWebFiles()).
//
// Only what changed (2026-10-21): a package still carries every web file,
// but the clock stores only those that differ from its own (size + CRC-32),
// so an update needs room only for what changed - the rest of the space is
// for event pictures. On install, every file the new version doesn't list
// is removed, and folders left empty - except the clock's own data
// (isClockDataPath() in Config.h: settings, events, logs, pictures...).
// =============================================================================

namespace Updater {
    void begin();              // in setup(): note whether this firmware is on trial
    bool justInstalled();      // this start is the first of a firmware just installed (on trial) - before begin() too
    void loop();               // every loop(): trial -> confirm / roll back; install waiting web files
    const char* stateName();   // "trial" | "installing" | "ok" - for /api/status
    // An update that didn't stay (its firmware failed its trial, crashed, or found no
    // Wi-Fi, and the clock went back by itself): its version (the user, 2026-11-14: so
    // the family knows to call). Settings: until "OK, seen" (failedUpdateSeen()) or an
    // update that stays; the screen's red status line: the same, at most
    // UPDATE_FAILED_SCREEN_DAYS from the day it first showed. "" = none.
    String failedUpdate();
    String failedUpdateOnScreen();
    void   failedUpdateSeen();
    bool movingWebFiles();     // a package's web files are being put in place right now

    // Receiving an upload (called from the /update handler, AsyncTCP task).
    // Checked with the setting's update key (UpdateKeyState.h) - none: nothing is installed.
    // erase: an OLDER version may be installed - everything of the clock's own is then erased
    // (but Wi-Fi), as an older version can't read a newer one's data (eraseWanted()).
    void   uploadBegin(const String& filename, size_t totalBytes, bool erase = false);
    bool   eraseWanted();      // after a successful uploadEnd(): erase before the restart
    void   uploadAbort(const String& why);   // the sender gives up (e.g. a download of the wrong size): nothing installed
    // An update area (LogLevel.h Logging::Focus) entered / left: the log at DEBUG while in any,
    // each change said in the log (what = nullptr: quietly). For the A -> B -> C phone test only - remove with it.
    void   debugFocus(uint8_t area, bool on, const char* what);
    void   uploadChunk(const uint8_t* data, size_t len);
    bool   uploadEnd();        // true = installed; the caller restarts the clock
    bool   uploadInProgress(); // data arriving (or restart pending) - main.cpp shows a still screen
    String uploadMessage();    // plain-language result for the update page
    // What the page acts on - never read from the wording (2026-11-20, the compliance run): sent as
    // headers X-Update-Version / X-Update-Refused: older (WebUI.cpp /update).
    String uploadVersion();    // the package's version, once read ("" before)
    bool   uploadRefusedOlder(); // refused as an older version (the page then offers "erase everything")

    // Minimal built-in update page, served when /update.html is missing or
    // broken - so a bad web-files update can always be fixed by another one.
    extern const char EMERGENCY_UPDATE_PAGE[];
}
