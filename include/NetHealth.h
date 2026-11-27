#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Is the internet there? (2026-11-21, the user: "no reply does not necessarily mean we are refused -
// it could be a real internet outage. Verify somewhere else whether it is the time server or the
// internet; if it is the internet, don't rest the time servers - keep watching the connection, and
// the moment we are online again, get the time.")
//
// Evidence, from any part of the clock: an answer from any server - an HTTP status (Hebcal, the
// place lookup, the update feed), a time server's reply - is heard(). When a time server is silent,
// NTPManager asks for a check at once (checkNow()): the download task makes one small request
// elsewhere (CONNECTIVITY_CHECK_URL, Config.h) and NTPManager decides on what it finds - anything
// heard since = the internet is there; the check failed = it isn't. While it isn't, probeWanted()
// repeats the check at most every PROBE_MIN_GAP_MS (the user, 2026-11-21: "cheap monitoring").
// ─────────────────────────────────────────────────────────────────────────────
#include <stdint.h>

namespace NetHealth {

void    heard();                       // a server answered us (any task)
bool    heardSince(int64_t uptimeMs);  // something answered at or after that moment
bool    checkedDownSince(int64_t uptimeMs);   // a check finished after that moment and found no internet
void    checkNow();                    // a check as soon as the download task can (a time server silent)
void    probeWanted();                 // the internet gone: check again - at most every PROBE_MIN_GAP_MS
void    probeInTask();                 // the download task (Calendar.cpp): the check, if wanted
bool    down();                        // the last check found no internet (until something answers)

}  // namespace NetHealth
