#pragma once

#include <Arduino.h>

// =============================================================================
// Updater.h
// JewishClock - installing updates over Wi-Fi, safely, with nobody technical
// on site (the clock lives at the author's parents; updates arrive by email
// and are installed from the settings page's Firmware Update page).
//
// What can be uploaded to POST /update:
//   - a plain firmware .bin (as before), or
//   - a .jcupdate package (utils/make_update_package.py): firmware + all the
//     web files from Data/ (never config.json / events.json / the log).
//
// Safety net:
//   - New firmware boots "on trial" (ESP-IDF app rollback). It confirms
//     itself only after joining Wi-Fi and staying up TRIAL_STABLE_MS. If it
//     crashes, hangs, or gets no Wi-Fi within TRIAL_CONNECT_DEADLINE_MS, the
//     clock goes back to the previous firmware by itself.
//   - A package's web files are set aside (/upd) and put in place only once
//     the firmware they belong to has passed its trial - so firmware and web
//     page can never end up from different versions. After a rollback the
//     waiting files are discarded.
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
    void loop();               // every loop(): trial -> confirm / roll back; install waiting web files
    const char* stateName();   // "trial" | "installing" | "ok" - for /api/status

    // Receiving an upload (called from the /update handler, AsyncTCP task).
    void   uploadBegin(const String& filename, size_t totalBytes);
    void   uploadChunk(const uint8_t* data, size_t len);
    bool   uploadEnd();        // true = installed; the caller restarts the clock
    bool   uploadInProgress(); // data arriving (or restart pending) - main.cpp shows a still screen
    String uploadMessage();    // plain-language result for the update page

    // Minimal built-in update page, served when /update.html is missing or
    // broken - so a bad web-files update can always be fixed by another one.
    extern const char EMERGENCY_UPDATE_PAGE[];
}
