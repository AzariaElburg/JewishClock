#pragma once

// =============================================================================
// Storage.h
// JewishClock - Persistent storage via LittleFS.
//
// ARCHITECTURE NOTE — why calibration lives in AppConfig / Storage:
//   Storage is the persistence layer for ALL user-configurable settings,
//   regardless of which subsystem uses them.  DisplayManager reads calibration
//   values from AppConfig (which Storage loaded from flash) — it never does
//   its own file I/O.  This is correct separation of concerns: Storage owns
//   persistence, Display owns rendering.
//
// WI-FI PASSWORD SECURITY:
//   wifiPassword is plaintext in RAM (needed by the Wi-Fi stack).
//   On flash it is stored XOR-encrypted; backups contain the encrypted form.
//   Key is firmware-embedded so backups are portable across devices.
// =============================================================================

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "Config.h"

// ─── Storage API ─────────────────────────────────────────────────────────────

class Storage {
public:
    // Call once in setup() before anything else accesses storage
    static bool begin();

    // ── Config ──
    static bool loadConfig(AppConfig& cfg);
    static bool saveConfig(const AppConfig& cfg);
    // For a change the clock made by itself (learned light, a network's place,
    // last network): saved by loop() when the screen stands still
    // (DisplayManager::goodMomentToWrite()). Any save before that covers it.
    // Main loop only.
    static void saveConfigSoon();
    static void loop(const AppConfig& cfg);
    static void applyDefaults(AppConfig& cfg);
    // Every number inside its LIMITS (Config.h); true if one had to move.
    // After each load and each change from the page or an import - whatever
    // wrote config.json, the clock runs only on values it was built for.
    static bool clampConfig(AppConfig& cfg);
    // The show windows (AppConfig::candleShow ...) to / from JSON keys
    // candleFrom, candleUntil, candleTomorrow, ...: read takes only the keys
    // present (loadConfig after applyDefaults, a change from the page).
    static void readShowWindows(JsonVariantConst doc, AppConfig& cfg);
    // Time servers from JSON (config.json, the page, a settings import). Checks
    // first and changes nothing if the list is wrong: returns the reason ("" = taken).
    // A list shorter than NTP_SERVER_SLOTS is from before 2026-11-13 (the page had
    // one box): the missing slots get the defaults not already in it.
    static String readNtpServers(JsonArrayConst list, AppConfig& cfg);
    // ── Texts read from files and requests (2026-11-13) ──
    // Every text the clock takes in has a limit: Config.h LIMITS where there is
    // one, else what its field holds. ONE list of them (configTextProblems /
    // eventProblems in Storage.cpp), used three ways - Mode:
    //   Change  - a settings change, an event saved or imported: refused, every
    //             problem said; a text equal to what the clock keeps passes.
    //   Restore - a backup: refused as a whole, every problem said.
    //   OnClock - the clock's own files (an older version's, or edited by hand):
    //             kept as far as the field holds, and a note for the page
    //             (textNotes(), /api/config "textNotes") until it is changed.
    // key: config key, "places.N", "event:<id>". field/detail (events' dates, 2026-11-14):
    // which numbers - "gD", "gD gY", "jM jY" ... - and the problem without "Event N (name): ",
    // so the page can name the Excel row and its columns instead.
    struct TextNote { String key; String text; String field = ""; String detail = ""; };
    enum class TextMode : uint8_t { Change, Restore, OnClock };
    static std::vector<TextNote> configTextProblems(JsonObjectConst doc, const AppConfig* cur, TextMode mode);
    // The clock's own Wi-Fi password: "" = usable, else why not (8-63 plain ASCII characters,
    // Config.h AP_PASSWORD_MIN/MAX_CHARS). The one check - Settings' Change and the clock's own.
    static String apPasswordProblem(const char* pw);
    // One event (JSON as in events.json / an import); number = its place in the
    // list, counted from 1 (0 = a single event, not numbered in the text).
    static void eventProblems(JsonObjectConst ev, int number, TextMode mode, std::vector<TextNote>& out);
    static String joinTexts(const std::vector<TextNote>& list);   // one per line
    // A settings change: "" = fine, else every problem (Mode Change).
    static String checkTexts(JsonObjectConst doc, const AppConfig& cur);
    // The notes from the clock's own files (any task).
    static std::vector<TextNote> textNotes();
    // Fixed: a note goes when its text is saved again. Exact key, or every key
    // starting with `prefix` ("event:", "places.2").
    static void clearTextNote(const String& key);
    static void clearTextNotes(const String& prefix);
    // The notes of the settings / the events gone (kept in TEXT_NOTES_PATH until their text
    // changes): they were replaced by a restore.
    static void forgetTextNotes(bool config, bool events);
    // The status slide's "Error - see Settings": notes there, and within their
    // days on the screen counted from the first day shown (today = local days since 1970).
    static bool textNotesOnScreen(long today);
    // Settings' "OK, seen" for them (the user, 2026-11-15): off the screen; Settings lists them on
    // until each is changed. A new problem is unseen again.
    static bool textNotesSeen();
    static void textNotesSeenNow();
    // "at most N characters (N/2 in Hebrew)" - the one wording of a text limit.
    static String textLimitText(int maxBytes, bool hebrew);
    static void writeShowWindows(JsonObject doc, const AppConfig& cfg);
    // The slides' order + frequencies from JSON (each key only if present); the
    // order always holds every type once (Storage.cpp).
    static void readContent(JsonVariantConst doc, AppConfig& cfg);
    // Factory reset: deletes everything the clock wrote itself
    // (isClockDataPath(): settings, events, pictures, logs, cache, ...) -
    // its own files (page, built-in pictures, tz table) stay - then writes
    // default settings, an empty event list, and a new log that starts with
    // the reset. The caller restarts the clock. Main loop only.
    // Everything of the clock's own erased, default settings. keepWifiFrom: these settings'
    // Wi-Fi networks and own-Wi-Fi password kept (an older version installed with "erase":
    // it must get on Wi-Fi for its trial); keepWaitingUpdate: its web files kept too.
    static void factoryReset(const AppConfig* keepWifiFrom = nullptr, bool keepWaitingUpdate = false);

    // ── Events ──
    // Replaces every element of `events` with what's on disk. Any event
    // missing an id (events.json written before ids existed) gets one
    // assigned here, once, as part of the load - see generateEventId().
    // `path`: FS_EVENTS_PATH, or FS_DELETED_EVENTS_PATH (the recycle bin -
    // same format; a missing file is an empty list).
    static bool loadEvents(EventList& events, const char* path = FS_EVENTS_PATH);
    static bool saveEvents(const EventList& events, const char* path = FS_EVENTS_PATH);

    // A 16-hex-digit id from the hardware RNG, guaranteed unique against
    // `existing` by construction: candidates are checked against every
    // current id and re-rolled on the (astronomically unlikely) collision -
    // see PersonalEvent::id's comment in Config.h for why this scheme was
    // chosen over a persisted counter.
    static String generateEventId(const EventList& existing);

    // ── Events <-> JSON: the one mapping (2026-11-14) ──
    // File = events.json and backups (jM: the Hebrew month's number); Page = the
    // web page's (jMonth: its name, isAdarII). Reading checks the type, the icon,
    // the picture ids and the dates (DateRules.h) whatever the source.
    enum class EventJson : uint8_t { File, Page };
    // Its date numbers as given (either shape), unchecked: 0 = not given.
    struct EventDate { int gD = 0, gM = 0, gY = 0, jD = 0, jM = 0, jY = 0; };
    static EventDate eventDateOf(JsonObjectConst in);
    static void eventFromJson(JsonObjectConst in, PersonalEvent& e, EventJson shape);
    static void eventToJson(const PersonalEvent& e, JsonObject out, EventJson shape);
    struct EventType { const char* type; const char* icon; };
    static const EventType* eventTypes(size_t& count);   // the last one: what an unknown type shows
    static String normalizeEventType(const String& raw);  // "" = not a known type
    static const char* defaultIconForType(const char* type);

    // ── Bundled backup (config + events in one JSON object) ──
    static String exportBackup();
    // Refused (false, `why` = every problem) if a text in it is too long - checked
    // before anything is written.
    static bool   importBackup(const String& json, String& why);

    // ── Individual section backup (used internally & by exportBackup) ──
    static String exportConfigBackup();
    static String exportEventsBackup();
    static bool   importConfigBackup(const String& json);

    // Encryption helpers (public for WebUI use)
    static String encryptPassword(const char* plaintext);
    static void   decryptPassword(const char* hex, char* out, size_t maxLen);

    // ── Utility ──
    static bool   exists(const char* path);
    static bool   remove(const char* path);
    static size_t freeBytes();
};
