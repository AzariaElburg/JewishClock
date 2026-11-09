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
    static void writeShowWindows(JsonObject doc, const AppConfig& cfg);
    // Factory reset: deletes everything the clock wrote itself
    // (isClockDataPath(): settings, events, pictures, logs, cache, ...) -
    // its own files (page, built-in pictures, tz table) stay - then writes
    // default settings, an empty event list, and a new log that starts with
    // the reset. The caller restarts the clock. Main loop only.
    static void factoryReset();

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

    // ── Bundled backup (config + events in one JSON object) ──
    static String exportBackup();
    static bool   importBackup(const String& json);

    // ── Individual section backup (used internally & by exportBackup) ──
    static String exportConfigBackup();
    static String exportEventsBackup();
    static bool   importConfigBackup(const String& json);
    static bool   importEventsBackup(const String& json);

    // Encryption helpers (public for WebUI use)
    static String encryptPassword(const char* plaintext);
    static void   decryptPassword(const char* hex, char* out, size_t maxLen);

    // ── Utility ──
    static bool   exists(const char* path);
    static bool   remove(const char* path);
    static size_t freeBytes();
};
