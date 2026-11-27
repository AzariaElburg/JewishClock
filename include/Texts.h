#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The clock's own words that tests look for - defined ONCE, here (the user,
// 2026-11-20). The clock's code uses them by name; the clock serves the same list
// at GET /api/texts (jcsim, jcclock: --texts), so a test asks "what does the clock
// say in this situation?" and gets exactly what the code uses - never a copy that
// can drift. A test asking for a name not listed fails, saying so
// (sim/clock_facts.py ClockWords). sim/scrambled_build.py proves it: every text
// scrambled, the tests still pass.
// Three groups, one namespace (a name used twice doesn't compile):
//   SCREEN - what the screen shows (system messages: English in every display language);
//   ANSWER - what the clock answers the page / a request (refusals, problems, states);
//   LOG    - log lines tests read (printf formats: %d, %s... - used with
//            DiagnosticLog::record() / CLOCK_LOGF(), never CLOCK_LOG: that one glues
//            a written-out format at compile time).
// {} = what the clock fills in, in order (fill()).
// A new text a test needs: a line here, used through Texts::.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <ArduinoJson.h>
#include <initializer_list>

#define SCREEN_TEXTS(X) \
    X(noInternet,       "No internet!") \
    X(noNtp,            "No NTP for {} days") \
    X(updateFailed,     "Update to {} failed") \
    X(errorSeeSettings, "Error - see Settings") \
    X(ntpServerRefused, "NTP server refused - see Settings") \
    X(clockWifi,        "Clock's Wi-Fi: {} - password {}") \
    X(partsJoin,        " - ") \
    X(waitingForTime,   "Waiting for the time...") \
    X(connecting,       "Connecting to {}...") \
    X(lookingForWifi,   "Looking for Wi-Fi...") \
    X(noWifiConfigured, "No Wi-Fi configured. Starting AP...") \
    X(setupLine,        "Setup: join Wi-Fi {} - password {} - open 192.168.4.1") \
    X(updating,         "Updating...") \
    X(loading,          "Loading...")

#define ANSWER_TEXTS(X) \
    X(ntpNotServerReply, "not a server's reply (mode {})") \
    X(ntpNotAnswer,      "not an answer to the clock's question (late, repeated or forged)") \
    X(ntpClockUnset,     "the server's own clock isn't set (stratum {})") \
    X(ntpClockAlarm,     "the server's own clock isn't set (alarm)") \
    X(ntpImpossibleTrip, "impossible travel time ({} ms)") \
    X(ntpKiss,           "kiss-o'-death {}") \
    X(updFailed,         "Update FAILED - {}.") \
    X(updNotSigned,      "the update file is not signed - only updates signed by the clock's maker are installed") \
    X(updNoKey,          "the clock has no update key, so it can't check this file - paste the key first (Settings > Clock software > Update key)") \
    X(updBadSignature,   "the update file's signature does not match - it was changed, or is not from the clock's maker") \
    X(updNoRoom,         "not enough room on the clock for this update's changed files - removing some event pictures makes room") \
    X(updDamagedFile,    "damaged file in the update ({}) - please download it again") \
    X(updMayNotReplace,  "the update contains a file it may not replace: {}") \
    X(updFirmwareInvalid, "the firmware in the update is not valid") \
    X(updFirmwareNotStarted, "the firmware could not be started ({})") \
    X(updUnknownEntry,   "unknown kind of entry in the update file") \
    X(updWriteFailed,    "writing the firmware failed ({})") \
    X(updNotAnUpdate,    "this is not a clock update file") \
    X(updDamaged,        "the update file is damaged") \
    X(updOlderVersion,   "this is an older version ({}) than the clock's ({}) - it can't read this version's settings and events; install it only with \"erase everything\" (then restore them from an Excel export)") \
    X(updCantReadData,   "version {} can't read this clock's data directly - install a version {}.x first") \
    X(updEmpty,          "the update file is empty") \
    X(updIncomplete,     "the update file is incomplete - please download it again") \
    X(updStalled,        "the upload stopped before it was complete - please try again") \
    X(updPlainFirmware,  "a plain firmware file has no signature - only update packages (.jcupdate) are installed") \
    X(updNoFirmware,     "the update file has no firmware in it") \
    X(updNotInstalled,   "the firmware in the update could not be installed ({})") \
    X(updNothing,        "nothing was uploaded") \
    X(updInstalling,     "Update OK. Installing version {}. Rebooting...") \
    X(initializing,      "{} is initializing...") \
    X(initializingWait,  "Synchronizing time and calendar data. Please wait.") \
    X(setupSaved,        "Settings Saved") \
    X(setupNotSaved,     "Not saved") \
    X(setupNeedName,     "Please enter the network's name.") \
    X(setupNameTooLong,  "The network name is too long: {}, the most Wi-Fi allows.") \
    X(setupPassTooLong,  "The password is too long: {}, the most Wi-Fi allows.") \
    X(savedRestarting,   "Saved - the clock restarts with its new name") \
    X(invalidJson,       "Invalid JSON") \
    X(invalidJsonArray,  "Invalid JSON array") \
    X(unknownProblem,    "Unknown problem") \
    X(needName,          "Please enter a name - who is this event for?") \
    X(debugYears,        "Choose a date between the years {} and {}") \
    X(notDateTime,       "Not a date and time: {}") \
    X(debugTimeExpected, "Expected offset, local or timestamp") \
    X(eventNotFound,     "Event not found") \
    X(notInBin,          "Not in the recycle bin") \
    X(noSuchPicture,     "No such picture") \
    X(iconsTooBig,       "Too big: replaced icons may use {} KB together, {} KB are left - a shorter animation fits") \
    X(ntpNeedOne,        "at least one time server is needed") \
    X(apPwNotChanged,    "Not changed - the password needs {}.") \
    X(apPwPlain,         "only English letters, digits and plain signs (no Hebrew, no accents)") \
    X(apPwSame,          "That is the password it has.") \
    X(wifiNotPlain,      "{}: only English letters, digits and plain signs (no Hebrew, no accents) - what every router takes") \
    X(atLeastChars,      "at least {} characters") \
    X(atMostChars,       "at most {} characters") \
    X(atMostCharsHeb,    "at most {} characters ({} in Hebrew)") \
    X(atMostLetters,     "at most {} letters and digits") \
    X(tooLong,           "{} is too long: {}.") \
    X(usesMeanwhile,     " The clock uses \"{}\" meanwhile.") \
    X(labelClockName,    "Clock name") \
    X(labelNetName,      "Network {} name") \
    X(labelNetPassword,  "Network {} password") \
    X(labelSeparator,    "Separator") \
    X(labelSetupNetName, "The network name") \
    X(labelSetupPassword, "The password") \
    X(labelEvent,        "Event {}") \
    X(labelEventAlone,   "Event") \
    X(labelEventName,    "{} name") \
    X(olNoVersions,      "No versions on the update feed yet") \
    X(olNoVersionsShort, "No versions on the feed yet") \
    X(olFeedMissing,     "The update feed is not there ({})") \
    X(olCouldNotAsk,     "Could not ask for updates ({})") \
    X(olAnswerCode,      "answer {}") \
    X(olNoConnection,    "no connection") \
    X(olNotReadable,     "The update feed's answer is not readable") \
    X(olDraft,           "a draft") \
    X(olBeingPutUp,      "its package still being put up") \
    X(olUnknownSize,     "its package of unknown size") \
    X(olAvailable,       "Version {} is available") \
    X(olLastTryFailed,   " - the last try failed: {}") \
    X(olNotReady,        "Version {} is not ready yet ({})") \
    X(olUpToDate,        "Up to date (newest: {})") \
    X(olPageInstalling,  "An update is being installed from the page") \
    X(olDownloading,     "Downloading version {}") \
    X(olDownloadFailed,  "The download failed ({})") \
    X(olWrongSize,       "the download brought {} bytes, the feed names {}") \
    X(olBrokeOff,        "The download broke off") \
    X(olInstalled,       "Version {} installed - restarting") \
    X(storageFull,       "Not saved: the clock's storage is full") \
    X(psramSavedLow,     "Saved, but only {}% of PSRAM remains free.") \
    X(psramEventRefused, "Not saved: only {}% of PSRAM free (refusing new events below {}%).") \
    X(psramImportRefused, "Not imported: only {}% of PSRAM free (refusing below {}%).") \
    X(eventsTooMany,     "the clock holds at most {} events - this list has {}") \
    X(eventsFileTooMany, "The events file has {} events - the clock holds at most {}; the first {} were kept") \
    X(ntpBadServerName,  "\"{}\" is not a time server name (letters, digits, dots and dashes, up to {})") \
    X(restoredEvents,    "Restored {} - the event list is full") \
    X(restoredCount,     "Restored {} events") \
    X(restored,          "Restored") \
    X(notSaved,          "Not saved: {}") \
    X(ntpTestNoAddress,  "no such server (its name has no address)") \
    X(ntpTestNoPort,     "the clock could not open a port for it") \
    X(ntpTestBadAnswer,  "it answered, but {}") \
    X(ntpTestNoAnswer,   "no answer from {}") \
    X(dateMismatchG,     "Date mismatch: {} is {}, but the Hebrew date says {}.") \
    X(dateMismatchH,     "Date mismatch: {} is {} (after sunset: the evening before), but the Gregorian date says {}.") \
    X(dateNeverMeet,     "Date mismatch: {} and {} never fall on the same day, in any year.") \
    X(afterSunset,       " after sunset") \
    X(importNoName,      "Not imported: event(s) {} have no name. Nothing was changed.") \
    X(notUpdateKey,      "Not an update key") \
    X(restoreRefused,    "Not restored - nothing was changed. {}.") \
    X(restoreSettingsOnly, "Not restored - nothing was changed. A restore takes settings only - events come in through the Excel import.") \
    X(restoreTooLong,    "Not restored - nothing was changed. Too long:\n{}") \
    X(gMonthRange,       "month {} - a month is 1 to 12") \
    X(noDay,             "{} has no day {} - it has {}") \
    X(febDays,           "28 days, 29 in a leap year") \
    X(nDays,             "{} days") \
    X(febNot29,          "February {} has 28 days, no 29th - {} is not a leap year") \
    X(hMonthRange,       "Hebrew month {} - a month is 1 (Tishrei) to 13 (Adar II)") \
    X(hDaysVary,         "29 or 30 days") \
    X(noAdarII,          "{} has no Adar II - it is not a leap year (one Adar)") \
    X(hYearDays,         "{} {} has {} days, no day {}") \
    X(yearFrom,          "a year from {} to {}") \
    X(gYearOut,          "Gregorian year {} - {}") \
    X(hYearOut,          "Hebrew year {} - {}") \
    X(notADate,          "Not a date: {}") \
    X(noRuleDay,         "the clock's date rules found no day for it in {} - please report this") \
    X(openToCorrect,     "Open the event and correct it.") \
    X(verUnstamped,      "{} doesn't name the clock software version that made it (files made before 2026-11-15) - too old for version {}. Import it on a clock with an older version, export it again there, and bring that file here") \
    X(verUnknown,        "{} names a version the clock doesn't know ({})") \
    X(verNewer,          "{} was made by version {}, newer than this clock's ({}) - update the clock first") \
    X(verTooOld,         "{} was made by version {} - version {} reads data of version {}.x and newer only. Put it on a clock with version {}.x first, export it there, then bring that file here")

#define LOG_TEXTS(X) \
    X(logFailing,        "%s failing: %s") \
    X(logWorksAgain,     "%s works again after %llu failed attempt(s)") \
    X(ntpReplyStreak,    "Time server reply") \
    X(hebcalStreak,      "Hebcal download") \
    X(zmanimStreak,      "Zmanim download") \
    X(geoStreak,         "Location lookup") \
    X(ntpAgreeStreak,    "Two time replies agreeing on a jump") \
    X(ntpLeft,           "Sync: %+lld ms - left as it is (round trip %lld ms)") \
    X(ntpGradual,        "Sync: %+lld ms - corrected gradually (round trip %lld ms)") \
    X(ntpJumpNotConfirmed, "a jump of %+.3f s not confirmed: %s says %+lld ms - no jump") \
    X(ntpStep,           "%s: %s UTC from %s%s, round trip %lld ms") \
    X(ntpFirst,          "first time") \
    X(ntpFirstConfirmed, "first time (confirmed)") \
    X(ntpClockSet,       "clock set (confirmed)") \
    X(ntpKissLog,        "%s answered kiss-o'-death \"%s\"") \
    X(ntpDroppedPacket,  "Dropped a packet from %s (%d bytes) - not the server asked") \
    X(updTrialPassed,    "new firmware %s passed its trial - kept") \
    X(updTrialFailed,    "new firmware %s failed its trial (no stable Wi-Fi) - going back to the previous version") \
    X(updAccepted,       "package accepted - restarting into its trial") \
    X(olFeed404,         "the feed answered 404; its repository %d") \
    X(olFeedAnswered,    "the feed answered %d, %u bytes") \
    X(olNotReadableLog,  "update feed answer not readable (%u bytes)") \
    X(olDownloadingLog,  "downloading update %s") \
    X(olNightInstall,    "automatic update at night") \
    X(olNotTaken,        "update %s on the feed not taken yet: %s") \
    X(olDownloadAnswered, "download answered %d, length %d") \
    X(focusOn,           "level debug by itself: %s (the setting, %s, back after)") \
    X(focusBack,         "level back to the setting (%s): %s") \
    X(focusDownloading,  "downloading an update") \
    X(focusDownloadOver, "download over") \
    X(focusOnTrial,      "new version on trial") \
    X(focusTrialOver,    "trial over, web files in place") \
    X(bmpNot16Bit,       "ERROR: '%s' is not a 16-bit BMP. Must be 16.") \
    X(bmpCompression,    "ERROR: '%s' has unsupported compression %d. Must be 0 or 3.") \
    X(bmpHeight,         "ERROR: BMP '%s' height %d is invalid. Must be 1-%dpx.") \
    X(bmpMasks,          "ERROR: BMP '%s' has unsupported BI_BITFIELDS masks (R:0x%X, G:0x%X, B:0x%X).") \
    X(noConfigFile,      "No config file \u2014 using defaults") \
    X(cacheOldFormat,    "Cache from an older format - ignored, will download") \
    X(hebcalUnusable,    "Hebcal fetch returned HTTP 200 but no usable calendar - keeping the old one, not caching.") \
    X(zmanimNoSunset,    "Zmanim response had no sunset times - will retry") \
    X(zmanimParseError,  "Zmanim parse error: %s") \
    X(ipLoss,            "Detected IP loss while in connected state. Re-initiating connection.") \
    X(revertToAp,        "Connection attempt timed out. Reverting to AP.") \
    X(roamScanFailed,    "roam scan: %s (code %d)") \
    X(roamScan,          "roam scan: on %s at %d dBm; seen %s-> %s") \
    X(signalWeak,        "signal weak: rssi=%d dBm") \
    X(signalRecovered,   "signal recovered: rssi=%d dBm after %lus weak (lowest %d dBm)") \
    X(wifiDisconnected,  "[EVENT] Disconnected (Reason: %d)") \
    X(wifiReconnected,   "reconnected #%u rssi=%d ap=%s ch=%d ip=%s") \
    X(ntpNextServer,     "%s: %s - trying %s next") \
    X(ntpSent,           "Packet sent to %s") \
    X(ntpRest,           "%s not asked for %lld s (%s)") \
    X(ntpAllResting,     "every time server resting or asked within the last minute: the next question in %lld s, to %s") \
    X(ntpAllRefused,     "every time server refused the clock (kiss-o'-death) - none asked until the list changes or the clock restarts") \
    X(ntpNetCheck,       "%s: %s - checking the internet before blaming it") \
    X(ntpMarked,         "%s not recommended from now (%s): asked last, once a day") \
    X(ntpStillTrouble,   "%s still not recommended (%s): its good days counted from zero again") \
    X(ntpGoodDay,        "%s (not recommended) answered well: good day %u of %u") \
    X(ntpTroubleCleared, "%s recommended again: %u good days") \
    X(ntpDailyCheck,     "the daily question to %s (not recommended, %u of %u good days)") \
    X(ntpTroubleFileBad, "%s unreadable - no time server marked") \
    X(ntpWhySilent,     "no answer") \
    X(ntpWhyNoAddress,  "no address for its name") \
    X(ntpWhyNotOurs,    "answered, never to the question asked") \
    X(ntpWhySecond,     "a second opinion") \
    X(ntpWhyBestFree,   "the best one free") \
    X(ntpDenied,         "%s answered kiss-o'-death %s: not asked again (until it leaves the list or the clock restarts)") \
    X(ntpBackAfterRest,  "%s answers again (after %s)") \
    X(ntpNotBlamed,      "%s: %s - and the internet doesn't answer either: not counted against it; waiting for the internet") \
    X(ntpInternetBack,   "the internet answers again - asking for the time now") \
    X(ntpNextRegular,    "the next regular question in %lld s") \
    X(ntpCountryPool,    "country pool: %s.pool.ntp.org") \
    X(ntpCountryPoolOff, "country pool cleared (using global pool.ntp.org)") \
    X(netStreak,         "Internet check") \
    X(netNone,           "nothing answered") \
    X(svcWait,           "%s asks the clock to wait %ld s (HTTP %d)") \
    X(timeSeenIgnored,   "Ignoring %s: \"%s\"") \
    X(calFetching,       "Fetch conditions met. Requesting in background: %s") \
    X(placeLookedUp,     "network %d looked up: %s, %s (%s)")

namespace Texts {

#define TEXT_CONST(name, text) constexpr const char* name = text;
SCREEN_TEXTS(TEXT_CONST)
ANSWER_TEXTS(TEXT_CONST)
LOG_TEXTS(TEXT_CONST)
#undef TEXT_CONST

// `tmpl` with each {} replaced by the next of `parts` (missing ones: "")
inline String fill(const char* tmpl, std::initializer_list<String> parts) {
    String out;
    auto it = parts.begin();
    for (const char* p = tmpl; *p; p++) {
        if (p[0] == '{' && p[1] == '}') { if (it != parts.end()) out += *it++; p++; }
        else out += *p;
    }
    return out;
}
inline String fill(const char* tmpl) { return fill(tmpl, {}); }
inline String fill(const char* tmpl, const String& a) { return fill(tmpl, { a }); }
inline String fill(const char* tmpl, const String& a, const String& b) { return fill(tmpl, { a, b }); }
inline String fill(const char* tmpl, const String& a, const String& b, const String& c) { return fill(tmpl, { a, b, c }); }

// {"noInternet": "No internet!", ...} - the whole list (GET /api/texts, --texts)
inline void toJson(JsonDocument& doc) {
#define TEXT_JSON(name, text) doc[#name] = text;
    SCREEN_TEXTS(TEXT_JSON)
    ANSWER_TEXTS(TEXT_JSON)
    LOG_TEXTS(TEXT_JSON)
#undef TEXT_JSON
}

}  // namespace Texts
