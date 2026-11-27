// =============================================================================
// Storage.cpp
// JewishClock - LittleFS persistent storage implementation.
// =============================================================================

#include "Storage.h"
#include "Uptime.h"
#include "DateRules.h"
#include "Config.h"
#include "Version.h"
#include "VersionRule.h"
#include "StringTools.h"
#include "NTPManager.h"
#include "JewishDateEngine.h"
#include "FileTools.h"
#include "DiagnosticLog.h"
#include "Display.h"
#include "LogLevel.h"
#include "ImageLibrary.h"
#include "Tunables.h"
#include "Texts.h"
#include <LittleFS.h>
#include <algorithm>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <atomic>
#include <mutex>
#include <cstdint>

const char* const CONTENT_TYPE_KEYS[(int)ContentType::COUNT] = {
    "events", "candle", "havdalah", "holiday", "mevarchim",
    "parasha", "omer", "dafyomi", "mishnayomit", "hebrewdate", "gregoriandate", "fast", "prayerswitch"
};

// =============================================================================
// Wi-Fi password encryption
// =============================================================================
//
// The Wi-Fi password is stored XOR-encrypted so it is never in plaintext on
// flash or in backup files.  This protects against casual inspection of backup
// JSON files (e.g. in a text editor or if a laptop is lost).
//
// The key is embedded in firmware, so:
//  • Different devices use the same key → backups are portable across devices.
//  • Restoring a backup in AP setup mode restores WiFi credentials instantly.

static const uint8_t WIFI_KEY[16] = {
    0x4A, 0x65, 0x77, 0x69, 0x73, 0x68, 0x43, 0x6C,
    0x6F, 0x63, 0x6B, 0x4B, 0x65, 0x79, 0x21, 0x3F
}; // "JewishClockKey!?"

static void replaceNotes(bool events, std::vector<Storage::TextNote> fresh);   // below, with the texts' list

// The slides' default order - matches the historical hardcoded build order in
// MessageAggregator. A saved order that doesn't name a type (older settings, a
// type added since) gets it where it stands here, after the saved ones.
static const ContentType kDefaultOrder[(int)ContentType::COUNT] = {
    ContentType::Events, ContentType::Candle, ContentType::Havdalah, ContentType::Holiday,
    ContentType::Mevarchim, ContentType::Parasha, ContentType::Omer, ContentType::DafYomi,
    ContentType::MishnaYomit, ContentType::HebrewDate, ContentType::GregorianDate,
    ContentType::Fast, ContentType::PrayerSwitch
};

static String normalizeProjectName(const String& raw) {
    return StringTools::normalize(raw, PROJECT_NAME_DEFAULT);
}

String Storage::encryptPassword(const char* plaintext) {
    String out;
    size_t len = strlen(plaintext);
    char buf[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02X",
                 (uint8_t)((uint8_t)plaintext[i] ^ WIFI_KEY[i & 15]));
        out += buf;
    }
    return out;
}

void Storage::decryptPassword(const char* hex, char* out, size_t maxLen) {
    out[0] = '\0';
    size_t hexLen = strlen(hex);
    if (hexLen == 0 || hexLen % 2 != 0) return;
    size_t byteLen = hexLen / 2;
    if (byteLen >= maxLen) byteLen = maxLen - 1;
    char nibbles[3] = {};
    for (size_t i = 0; i < byteLen; i++) {
        nibbles[0] = hex[i * 2];
        nibbles[1] = hex[i * 2 + 1];
        out[i] = (char)((uint8_t)strtol(nibbles, nullptr, 16) ^ WIFI_KEY[i & 15]);
    }
    out[byteLen] = '\0';
}

// =============================================================================
// begin()
// =============================================================================

bool Storage::begin() {
    if (!LittleFS.begin(true)) {
        return false;
    }
    CLOCK_LOG("Storage", "LittleFS mounted. Free: %u bytes", freeBytes());

    // Add the new listing logic here
    CLOCK_LOG("Storage", "--- Filesystem Listing ---");
    FileTools::listDirRecursive("/", "");
    CLOCK_LOG("Storage", "--------------------------");

    return true;
}


// =============================================================================
// factoryReset()
// =============================================================================

// Every file and folder under `dir`, folders after their contents.
static void listAll(const String& dir, std::vector<String>& files, std::vector<String>& dirs) {
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String p = f.path();
        bool isDir = f.isDirectory();
        f.close();
        if (isDir) { listAll(p, files, dirs); dirs.push_back(p); }
        else files.push_back(p);
    }
}

void Storage::factoryReset(const AppConfig* keepWifiFrom, bool keepWaitingUpdate) {
    DiagnosticLog::discardPending();   // lines waiting for the log file go with it
    std::vector<String> files, dirs;
    listAll("/", files, dirs);
    int removed = 0;
    auto waiting = [&](const String& p) { return keepWaitingUpdate && (p == FS_UPDATE_DIR || p.startsWith(String(FS_UPDATE_DIR) + "/")); };
    for (const String& p : files) if (isClockDataPath(p) && !waiting(p) && LittleFS.remove(p)) removed++;
    for (const String& p : dirs)  if (isClockDataPath(p) && !waiting(p)) LittleFS.rmdir(p);
    AppConfig def;
    applyDefaults(def);
    if (keepWifiFrom) {
        StringTools::utf8Copy(def.wifiSSID, keepWifiFrom->wifiSSID);
        StringTools::utf8Copy(def.wifiPassword, keepWifiFrom->wifiPassword);
        for (int i = 0; i < WIFI_NETWORKS - 1; i++) {
            StringTools::utf8Copy(def.extraWifi[i].ssid, keepWifiFrom->extraWifi[i].ssid);
            StringTools::utf8Copy(def.extraWifi[i].password, keepWifiFrom->extraWifi[i].password);
        }
        def.wifiLastNetwork = keepWifiFrom->wifiLastNetwork;
        StringTools::utf8Copy(def.apPassword, keepWifiFrom->apPassword);
        def.setupComplete = keepWifiFrom->setupComplete;
    }
    saveConfig(def);
    EventList none;
    saveEvents(none);
    // The log was among the files removed: this is its first line now.
    DiagnosticLog::record("FACTORY", "factory reset (firmware %s): %d files of the clock's own erased, default settings written",
                          FIRMWARE_VERSION, removed);
}

// =============================================================================
// applyDefaults()
// =============================================================================

void Storage::applyDefaults(AppConfig& cfg) {
    memset(&cfg, 0, sizeof(cfg));

    String normalizedName = normalizeProjectName(PROJECT_NAME_DEFAULT);
    StringTools::utf8Copy(cfg.projectName, normalizedName.c_str());
    cfg.wifiSSID[0]     = '\0';
    cfg.wifiPassword[0] = '\0';
    cfg.wifiLastNetwork = -1;

    // Until the first network's place is known (Location.cpp): Jerusalem,
    // the same values loadConfig() falls back to.
    StringTools::utf8Copy(cfg.locationCountry, "Israel");
    StringTools::utf8Copy(cfg.locationCity, "Jerusalem");
    cfg.latitude  = DEFAULT_LATITUDE;
    cfg.longitude = DEFAULT_LONGITUDE;
    StringTools::utf8Copy(cfg.tzid, "Asia/Jerusalem");
    StringTools::utf8Copy(cfg.tzPOSIX, DEFAULT_TZ_POSIX);
    cfg.inIsrael  = DEFAULT_IN_ISRAEL;
    cfg.rolloverMode = ROLLOVER_MODE_DEFAULT;
    memset(cfg.places, 0, sizeof(cfg.places));   // no network's place known yet

    cfg.use24Hour      = true;
    cfg.autoUpdate     = true;
    cfg.updateFeed[0]  = '\0';
    cfg.updateKey[0]   = '\0';
    cfg.updateKeyBy[0] = '\0';
    static_assert(NTP_SERVER_SLOTS == 3, "defaults below: one per slot");
    cfg.ntpServerCount = NTP_SERVER_SLOTS;
    StringTools::utf8Copy(cfg.ntpServers[0], NTP_SERVER_1);
    StringTools::utf8Copy(cfg.ntpServers[1], NTP_SERVER_2);
    StringTools::utf8Copy(cfg.ntpServers[2], NTP_SERVER_3);
    cfg.manualTimeSet  = false;

    StringTools::utf8Copy(cfg.language, LANG_DEFAULT);

    cfg.debugTimeOffset = 0;
    cfg.rotationMode    = ROTATION_MODE_DEFAULT;
    cfg.messageDwellSec = MESSAGE_DWELL_SEC_DEFAULT;
    StringTools::utf8Copy(cfg.messageSeparator, MESSAGE_SEPARATOR_DEFAULT);
    cfg.scrollSpeed     = SCROLL_SPEED_DEFAULT;
    cfg.fontCharGapPx   = FONT_CHAR_GAP_DEFAULT;
    cfg.fontWordGapPx   = FONT_WORD_GAP_DEFAULT;
    cfg.fontWidthScalePct = FONT_WIDTH_SCALE_DEFAULT;

    cfg.brightPct        = BRIGHT_PCT_DEFAULT;
    cfg.useLightSensor   = USE_LIGHT_SENSOR_DEFAULT;
    cfg.nightPct         = NIGHT_PCT_DEFAULT;
    cfg.dayPct           = DAY_PCT_DEFAULT;
    cfg.roomDarkPct      = ROOM_DARK_PCT_DEFAULT;
    cfg.roomLightPct     = ROOM_LIGHT_PCT_DEFAULT;
    cfg.learnRoomLight   = LEARN_ROOM_LIGHT_DEFAULT;

    cfg.showHebrewYear        = false; // Day + month is enough by default; year available as an opt-in
    cfg.showShabbatShalom     = false;
    // Calibration: use the measured physical window defaults (not the old VISIBLE_* values)
    cfg.calibX = DEFAULT_CALIB_X;
    cfg.calibY = DEFAULT_CALIB_Y;
    cfg.calibW = DEFAULT_CALIB_W;
    cfg.calibH = DEFAULT_CALIB_H;
    cfg.calibTearUs = DEFAULT_CALIB_TEAR_US;

    cfg.showRoshChodesh       = true;
    cfg.showFasts             = true;
    cfg.omerMode              = OMER_MODE_DEFAULT;
    cfg.showSpecialShabbat    = true;
    cfg.showLiturgical        = true;
    cfg.showSwitchoverPhrases = true;
    cfg.switchoverWindowDays  = SWITCHOVER_DISPLAY_DAYS;
    cfg.nusach                = NUSACH_DEFAULT;
    cfg.eventDayChange        = EVENT_DAY_CHANGE_DEFAULT;
    cfg.candleShow    = CANDLE_SHOW_DEFAULT;
    cfg.havdalahShow  = HAVDALAH_SHOW_DEFAULT;
    cfg.fastShow      = FAST_SHOW_DEFAULT;
    cfg.parashaShow   = PARASHA_SHOW_DEFAULT;
    cfg.mevarchimShow = MEVARCHIM_SHOW_DEFAULT;

    // Default order: kDefaultOrder (top of the file); default frequencies match
    // the old individual show* defaults (1 = every round/on, 0 = off).
    for (int i = 0; i < (int)ContentType::COUNT; i++) cfg.contentOrder[i] = (uint8_t)kDefaultOrder[i];
    cfg.contentFrequency[(int)ContentType::Events]      = 1;
    cfg.contentFrequency[(int)ContentType::Candle]      = 1;
    cfg.contentFrequency[(int)ContentType::Havdalah]    = 1;
    cfg.contentFrequency[(int)ContentType::Holiday]     = 1;
    cfg.contentFrequency[(int)ContentType::Mevarchim]   = SHOW_MEVARCHIM_DEFAULT ? 1 : 0;
    cfg.contentFrequency[(int)ContentType::Parasha]     = 1;
    cfg.contentFrequency[(int)ContentType::Omer]        = 1;
    cfg.contentFrequency[(int)ContentType::DafYomi]     = SHOW_DAF_YOMI_DEFAULT ? 1 : 0;
    cfg.contentFrequency[(int)ContentType::MishnaYomit] = SHOW_MISHNA_YOMIT_DEFAULT ? 1 : 0;
    cfg.contentFrequency[(int)ContentType::HebrewDate]  = 1;
    cfg.contentFrequency[(int)ContentType::GregorianDate] = 1;
    cfg.contentFrequency[(int)ContentType::Fast]          = 1;
    cfg.contentFrequency[(int)ContentType::PrayerSwitch]  = 1;

    cfg.webPasswordEnabled = false;
    cfg.webPassword[0]     = '\0';
    cfg.setupComplete      = false;

    StringTools::utf8Copy(cfg.logLevel, "info");
    cfg.logLevelUntil = 0;
    cfg.apPassword[0] = '\0';
}

// =============================================================================
// loadConfig()
// =============================================================================

bool Storage::loadConfig(AppConfig& cfg) {
    applyDefaults(cfg);  // Always start from defaults, then overlay saved values

    if (!exists(FS_CONFIG_PATH)) {
        CLOCK_LOGF("Storage", Texts::noConfigFile);
        return true;
    }
    File f = LittleFS.open(FS_CONFIG_PATH, "r");
    if (!f) {
        DiagnosticLog::record("Storage", "Cannot open the settings file - using defaults");
        return false;
    }

    FileTools::skipUtf8ByteOrderMark(f);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        DiagnosticLog::record("Storage", "Settings file unreadable (%s) - using defaults", err.c_str());
        return false;
    }
    // Every text against its limit: too long -> kept as far as its field holds,
    // and a note for the page (Storage.h "Texts read from files and requests").
    std::vector<TextNote> notes = configTextProblems(doc.as<JsonObjectConst>(), nullptr, TextMode::OnClock);

    // Load the obfuscated passwords. If missing, WiFi.begin will fail
    // gracefully and trigger the AP portal where the user can re-enter it.
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        StringTools::utf8Cut(wifiSsidAt(cfg, i), sizeof(cfg.wifiSSID), doc[wifiConfigKey("wifiSSID", i)] | "", sizeof(cfg.wifiSSID) - 1);
        const char* enc = doc[wifiConfigKey("wifiPassword", i)];
        if (enc) decryptPassword(enc, wifiPasswordAt(cfg, i), sizeof(cfg.wifiPassword));
        else     wifiPasswordAt(cfg, i)[0] = '\0';
    }
    cfg.wifiLastNetwork = doc["wifiLastNetwork"] | -1;
    // Never a password in any log, at any level, in any form (the user, 2026-11-17):
    // the "encrypted" one is XOR with a key in the source - readable by anyone.
    if (doc["wifiPassword"].is<const char*>())
        CLOCK_LOG("Storage", "Loaded Wi-Fi credentials for SSID: %s", (const char*)cfg.wifiSSID);

    String rawProjectName = doc["projectName"] | PROJECT_NAME_DEFAULT;
    StringTools::utf8Copy(cfg.projectName, normalizeProjectName(rawProjectName).c_str());

    StringTools::utf8Copy(cfg.locationCountry, doc["country"] | "Israel");
    StringTools::utf8Copy(cfg.locationCity,    doc["city"]    | "Jerusalem");
    cfg.latitude  = doc["lat"]      | DEFAULT_LATITUDE;
    cfg.longitude = doc["lon"]      | DEFAULT_LONGITUDE;
    StringTools::utf8Copy(cfg.tzid, doc["tzid"] | "Asia/Jerusalem");
    StringTools::utf8Copy(cfg.tzPOSIX, doc["tzPOSIX"] | DEFAULT_TZ_POSIX);
    cfg.rolloverMode = static_cast<RolloverMode>(doc["rolloverMode"] | static_cast<int>(ROLLOVER_MODE_DEFAULT));
    cfg.inIsrael  = doc["inIsrael"] | DEFAULT_IN_ISRAEL;
    // Each network's place: "places": [ {set, confirmed, city, country, lat,
    // lon, tzid, tzPOSIX, offsetSec}, ... ] - one per Wi-Fi slot.
    memset(cfg.places, 0, sizeof(cfg.places));
    JsonArray placesArr = doc["places"];
    for (int i = 0; i < WIFI_NETWORKS && placesArr && i < (int)placesArr.size(); i++) {
        JsonObject o = placesArr[i];
        Place& p = cfg.places[i];
        p.set       = o["set"] | false;
        p.confirmed = o["confirmed"] | false;
        StringTools::utf8Copy(p.city,        o["city"]        | "");
        StringTools::utf8Copy(p.country,     o["country"]     | "");
        StringTools::utf8Copy(p.countryCode, o["countryCode"] | "");
        p.lat = o["lat"] | 0.0f;
        p.lon = o["lon"] | 0.0f;
        StringTools::utf8Copy(p.tzid,    o["tzid"]    | "");
        StringTools::utf8Copy(p.tzPOSIX, o["tzPOSIX"] | "");
        p.offsetSec = o["offsetSec"] | 0;
    }
    cfg.use24Hour = doc["use24Hour"] | true;
    cfg.autoUpdate = doc["autoUpdate"] | true;
    StringTools::utf8Copy(cfg.updateFeed, doc["updateFeed"] | "");
    StringTools::utf8Copy(cfg.updateKey, doc["updateKey"] | "");
    StringTools::utf8Copy(cfg.updateKeyBy, doc["updateKeyBy"] | "");

    cfg.debugTimeOffset = doc["debugTimeOffset"] | (int64_t)0;
    NTPManager::setDebugOffset(cfg.debugTimeOffset);

    // Time servers: a wrong list is left out (clampConfig() then keeps the defaults).
    JsonArrayConst ntpArr = doc["ntpServers"];
    if (ntpArr) {
        String why = readNtpServers(ntpArr, cfg);
        if (why.length()) notes.push_back({"ntpServers", "Time servers in the settings file not used (" + why + "). The clock uses the default ones. Enter them again (Advanced)."});
    }

    StringTools::utf8Copy(cfg.language, doc["language"] | LANG_DEFAULT);

    cfg.rotationMode    = static_cast<RotationMode>(doc["rotationMode"] | static_cast<int>(ROTATION_MODE_DEFAULT));
    cfg.messageDwellSec = doc["messageDwellSec"] | MESSAGE_DWELL_SEC_DEFAULT;
    StringTools::utf8Copy(cfg.messageSeparator, doc["separator"] | MESSAGE_SEPARATOR_DEFAULT);
    cfg.scrollSpeed     = doc["scrollSpeed"] | SCROLL_SPEED_DEFAULT;
    cfg.fontCharGapPx   = doc["fontCharGapPx"] | FONT_CHAR_GAP_DEFAULT;
    cfg.fontWordGapPx   = doc["fontWordGapPx"] | FONT_WORD_GAP_DEFAULT;
    cfg.fontWidthScalePct = doc["fontWidthScalePct"] | FONT_WIDTH_SCALE_DEFAULT;

    cfg.brightPct        = doc["brightPct"]        | BRIGHT_PCT_DEFAULT;
    cfg.useLightSensor   = doc["useLightSensor"]   | USE_LIGHT_SENSOR_DEFAULT;
    cfg.nightPct         = doc["nightPct"]         | NIGHT_PCT_DEFAULT;
    cfg.dayPct           = doc["dayPct"]           | DAY_PCT_DEFAULT;
    cfg.roomDarkPct      = doc["roomDarkPct"]      | ROOM_DARK_PCT_DEFAULT;
    cfg.roomLightPct     = doc["roomLightPct"]     | ROOM_LIGHT_PCT_DEFAULT;
    cfg.learnRoomLight   = doc["learnRoomLight"]   | LEARN_ROOM_LIGHT_DEFAULT;

    cfg.showHebrewYear        = doc["showHebrewYear"]      | false;
    cfg.showShabbatShalom     = doc["showShabbatShalom"]  | false;
    cfg.calibX = doc["calibX"] | DEFAULT_CALIB_X;
    cfg.calibY = doc["calibY"] | DEFAULT_CALIB_Y;
    cfg.calibW = doc["calibW"] | DEFAULT_CALIB_W;
    cfg.calibH = doc["calibH"] | DEFAULT_CALIB_H;
    cfg.calibTearUs = doc["calibTearUs"] | DEFAULT_CALIB_TEAR_US;

    cfg.showRoshChodesh       = doc["showRoshChodesh"]    | true;
    cfg.showFasts             = doc["showFasts"]          | true;
    cfg.omerMode              = static_cast<OmerMode>(doc["omerMode"] | static_cast<int>(OMER_MODE_DEFAULT));
    cfg.showSpecialShabbat    = doc["showSpecialShabbat"] | true;
    cfg.showLiturgical        = doc["showLiturgical"]     | true;
    cfg.showSwitchoverPhrases = doc["showSwitchover"]     | true;
    cfg.nusach                = doc["nusach"].as<int>() == 1 ? Nusach::Sefard : Nusach::Ashkenaz;
    cfg.eventDayChange        = doc["eventDayChange"].as<int>() == 1 ? EventDayChange::Midnight : EventDayChange::WithHebrewDay;
    cfg.candleShow    = CANDLE_SHOW_DEFAULT;      // keys missing (files before 2026-11-08): as it always was
    cfg.havdalahShow  = HAVDALAH_SHOW_DEFAULT;
    cfg.fastShow      = FAST_SHOW_DEFAULT;
    cfg.parashaShow   = PARASHA_SHOW_DEFAULT;
    cfg.mevarchimShow = MEVARCHIM_SHOW_DEFAULT;
    readShowWindows(doc.as<JsonVariantConst>(), cfg);
    cfg.switchoverWindowDays  = doc["prayerSwitchDays"]   | SWITCHOVER_DISPLAY_DAYS;   // new key (2026-10-17): the old "switchoverDays" was an unused setting whose saved 0 meant something else

    // Order is a list of type keys, frequency a key->int map. Either one
    // missing (or a type the saved lists don't mention) keeps the
    // applyDefaults() value.
    readContent(doc.as<JsonVariantConst>(), cfg);
    // Hiding the Omer is its content row's "Off" since 2026-10-19 (the
    // wording choice moved into that row, without an Off of its own): an old
    // omerMode Off becomes exactly that, with the default wording.
    if (cfg.omerMode == OmerMode::Off) {
        cfg.contentFrequency[(int)ContentType::Omer] = 0;
        cfg.omerMode = OMER_MODE_DEFAULT;
    }

    cfg.webPasswordEnabled = doc["pwdEnabled"] | false;
    StringTools::utf8Copy(cfg.webPassword, doc["pwd"] | "");
    cfg.setupComplete = doc["setupComplete"] | false;

    StringTools::utf8Copy(cfg.logLevel, doc["logLevel"] | "info");
    cfg.logLevelUntil = doc["logLevelUntil"] | (int64_t)0;
    StringTools::utf8Copy(cfg.apPassword, doc["apPassword"] | "");

    if (clampConfig(cfg)) DiagnosticLog::record("Storage", "settings outside their limits were corrected (Config.h LIMITS)");
    replaceNotes(false, notes);
    CLOCK_LOG("Storage", "Config loaded OK");
    return true;
}

// The five show windows (Config.h ShowSpan): key prefix, setting, default,
// the "from" choices that slide offers.
struct ShowKey { const char* name; ShowSpan AppConfig::* field; ShowSpan def; unsigned fromChoices; };
static const ShowKey SHOW_KEYS[] = {
    { "candle",    &AppConfig::candleShow,    CANDLE_SHOW_DEFAULT,    SHOW_FROM_TIMED },
    { "havdalah",  &AppConfig::havdalahShow,  HAVDALAH_SHOW_DEFAULT,  SHOW_FROM_TIMED },
    { "fast",      &AppConfig::fastShow,      FAST_SHOW_DEFAULT,      SHOW_FROM_TIMED },
    { "parasha",   &AppConfig::parashaShow,   PARASHA_SHOW_DEFAULT,   SHOW_FROM_PARASHA },
    { "mevarchim", &AppConfig::mevarchimShow, MEVARCHIM_SHOW_DEFAULT, SHOW_FROM_MEVARCHIM },
};

void Storage::readShowWindows(JsonVariantConst doc, AppConfig& cfg) {
    char key[24];
    for (const ShowKey& k : SHOW_KEYS) {
        ShowSpan& w = cfg.*k.field;
        snprintf(key, sizeof(key), "%sFrom", k.name);
        if (doc[key].is<int>()) w.from = static_cast<ShowFrom>(doc[key].as<int>());
        snprintf(key, sizeof(key), "%sUntil", k.name);
        if (doc[key].is<int>()) w.until = static_cast<ShowUntil>(doc[key].as<int>());
        snprintf(key, sizeof(key), "%sTomorrow", k.name);
        if (doc[key].is<bool>()) w.tomorrow = doc[key].as<bool>();
    }
}

// The slides' order and frequencies (keys "contentOrder", "contentFrequency"),
// each only if present - config.json, a settings change, an import. The order
// is always every type exactly once (2026-11-14: it used to be filled slot by
// slot - an unknown or repeated name, or a list without a type, could show one
// slide twice a round and drop another for good): the listed ones in their
// order, unknown names and repeats skipped, then the missing ones as in
// kDefaultOrder.
void Storage::readContent(JsonVariantConst doc, AppConfig& cfg) {
    if (doc["contentOrder"].is<JsonArrayConst>()) {
        bool have[(int)ContentType::COUNT] = {};
        int n = 0;
        auto put = [&](int t) { if (!have[t]) { have[t] = true; cfg.contentOrder[n++] = (uint8_t)t; } };
        for (JsonVariantConst v : doc["contentOrder"].as<JsonArrayConst>()) {
            const char* key = v.as<const char*>();
            for (int t = 0; key && t < (int)ContentType::COUNT; t++)
                if (strcmp(key, CONTENT_TYPE_KEYS[t]) == 0) { put(t); break; }
        }
        for (ContentType t : kDefaultOrder) put((int)t);
    }
    JsonObjectConst freq = doc["contentFrequency"];
    for (int t = 0; freq && t < (int)ContentType::COUNT; t++)
        if (freq[CONTENT_TYPE_KEYS[t]].is<int>()) cfg.contentFrequency[t] = freq[CONTENT_TYPE_KEYS[t]].as<int>();
}

void Storage::writeShowWindows(JsonObject doc, const AppConfig& cfg) {
    char key[24];
    for (const ShowKey& k : SHOW_KEYS) {
        const ShowSpan& w = cfg.*k.field;
        snprintf(key, sizeof(key), "%sFrom", k.name);     doc[key] = static_cast<int>(w.from);
        snprintf(key, sizeof(key), "%sUntil", k.name);    doc[key] = static_cast<int>(w.until);
        snprintf(key, sizeof(key), "%sTomorrow", k.name); doc[key] = w.tomorrow;
    }
}

// A choice the slide doesn't offer goes back to its default.
static bool clampShowWindows(AppConfig& cfg) {
    bool moved = false;
    for (const ShowKey& k : SHOW_KEYS) {
        ShowSpan& w = cfg.*k.field;
        int f = static_cast<int>(w.from), u = static_cast<int>(w.until);
        if (f < 0 || f > 31 || !(k.fromChoices & (1u << f))) { w.from = k.def.from; moved = true; }
        if (u != (int)ShowUntil::AtTime && u != (int)ShowUntil::Midnight) { w.until = k.def.until; moved = true; }
    }
    return moved;
}

String Storage::readNtpServers(JsonArrayConst list, AppConfig& cfg) {
    if ((int)list.size() > NTP_SERVER_SLOTS) return "more than " + String(NTP_SERVER_SLOTS) + " time servers";
    char got[NTP_SERVER_SLOTS][NTP_SERVER_MAX_CHARS + 1] = {};
    int n = 0;
    bool any = false;
    for (JsonVariantConst v : list) {
        String s = v.is<const char*>() ? String(v.as<const char*>()) : String();
        s.trim();
        if (s.length() && !NTPManager::serverNameValid(s.c_str()))
            return Texts::fill(Texts::ntpBadServerName, s, String(NTP_SERVER_MAX_CHARS));
        StringTools::utf8Copy(got[n++], s.c_str());
        any |= s.length() > 0;
    }
    // Before 2026-11-13 the list had one entry (the page's only box): the slots
    // it never had get the defaults not already in it - a fallback for clocks
    // updated from 2.1.x.
    const char* defs[] = { NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3 };
    for (int i = n, d = 0; i < NTP_SERVER_SLOTS; i++) {
        for (; d < 3 && !got[i][0]; d++) {
            bool have = false;
            for (int j = 0; j < i; j++) have |= strcasecmp(got[j], defs[d]) == 0;
            if (!have) StringTools::utf8Copy(got[i], defs[d]);
        }
        any |= got[i][0] != '\0';
    }
    if (!any) return Texts::ntpNeedOne;
    memcpy(cfg.ntpServers, got, sizeof(got));
    cfg.ntpServerCount = NTP_SERVER_SLOTS;
    return "";
}

bool Storage::clampConfig(AppConfig& cfg) {
    bool moved = false;
    auto fix = [&moved](int& v, IntRange r) { int c = v < r.lo ? r.lo : v > r.hi ? r.hi : v; moved |= c != v; v = c; };
    auto fixf = [&moved](float& v, IntRange r) { float c = v < r.lo ? r.lo : v > r.hi ? r.hi : v; moved |= c != v; v = c; };
    fix(cfg.brightPct, PERCENT_RANGE);
    fixf(cfg.nightPct, PERCENT_RANGE); fixf(cfg.dayPct, PERCENT_RANGE);
    fixf(cfg.roomDarkPct, PERCENT_RANGE); fixf(cfg.roomLightPct, PERCENT_RANGE);
    fix(cfg.messageDwellSec, MESSAGE_DWELL_SEC_RANGE);
    fix(cfg.scrollSpeed, SCROLL_SPEED_RANGE);
    fix(cfg.fontCharGapPx, FONT_CHAR_GAP_RANGE);
    fix(cfg.fontWordGapPx, FONT_WORD_GAP_RANGE);
    fix(cfg.fontWidthScalePct, FONT_WIDTH_SCALE_RANGE);
    // The window: its size kept, its place moved so all of it is on the
    // screen (shrinking it instead lost the size for good when the place
    // moved back).
    fix(cfg.calibW, CALIB_W_RANGE);
    fix(cfg.calibH, CALIB_H_RANGE);
    fix(cfg.calibX, { CALIB_X_RANGE.lo, std::min(CALIB_X_RANGE.hi, (int)TFT_WIDTH - cfg.calibW) });
    fix(cfg.calibY, { CALIB_Y_RANGE.lo, std::min(CALIB_Y_RANGE.hi, (int)TFT_HEIGHT - cfg.calibH) });
    fix(cfg.calibTearUs, CALIB_TEAR_US_RANGE);
    fix(cfg.switchoverWindowDays, PRAYER_SWITCH_DAYS_RANGE);
    moved |= clampShowWindows(cfg);
    // Time servers: a name the clock can't use is dropped; none left -> the defaults.
    bool anyNtp = false;
    for (int i = 0; i < NTP_SERVER_SLOTS; i++) {
        if (cfg.ntpServers[i][0] && !NTPManager::serverNameValid(cfg.ntpServers[i])) { cfg.ntpServers[i][0] = '\0'; moved = true; }
        anyNtp |= cfg.ntpServers[i][0] != '\0';
    }
    if (!anyNtp) {
        StringTools::utf8Copy(cfg.ntpServers[0], NTP_SERVER_1);
        StringTools::utf8Copy(cfg.ntpServers[1], NTP_SERVER_2);
        StringTools::utf8Copy(cfg.ntpServers[2], NTP_SERVER_3);
        moved = true;
    }
    cfg.ntpServerCount = NTP_SERVER_SLOTS;
    // Texts (LIMITS): one longer than the limit can only come from an older
    // version or a file edited by hand. It is kept as it is - never shortened
    // behind the family's back; the page shows it and asks for a shorter one
    // (index.html checkKeptTexts()). Only a letter an older version's fixed-size
    // copy left half-written at the end is dropped (broken text, not shorter text).
    using StringTools::utf8Copy;
    moved |= utf8Copy(cfg.projectName, cfg.projectName);
    moved |= utf8Copy(cfg.messageSeparator, cfg.messageSeparator);
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        moved |= StringTools::utf8Cut(wifiSsidAt(cfg, i), sizeof(cfg.wifiSSID), wifiSsidAt(cfg, i), sizeof(cfg.wifiSSID) - 1);
        moved |= StringTools::utf8Cut(wifiPasswordAt(cfg, i), sizeof(cfg.wifiPassword), wifiPasswordAt(cfg, i), sizeof(cfg.wifiPassword) - 1);
    }
    return moved;
}

// "a Hebrew letter counts as two" - how the limits are told everywhere: bytes of UTF-8.
String Storage::textLimitText(int maxBytes, bool hebrew) {
    return hebrew ? Texts::fill(Texts::atMostCharsHeb, String(maxBytes), String(maxBytes / 2)) : Texts::fill(Texts::atMostChars, String(maxBytes));
}

// ─── Texts: one list of every text the clock takes in, and its limit ─────────
// (Storage.h "Texts read from files and requests").
namespace {
enum class Kind : uint8_t { Plain, Hebrew, Letters, Secret };

// A long value shown in a message: whole letters, at most this many bytes.
static constexpr size_t SHOWN_MAX_BYTES = 200;

String shown(const char* v) {
    char buf[SHOWN_MAX_BYTES + 1];
    bool cut = StringTools::utf8Cut(buf, sizeof(buf), v, SHOWN_MAX_BYTES);
    return String(buf) + (cut ? "..." : "");
}

String limitWords(int limit, Kind k) {
    if (k == Kind::Letters) return Texts::fill(Texts::atMostLetters, String(limit));
    return Storage::textLimitText(limit, k == Kind::Hebrew);
}

struct Checker {
    Storage::TextMode mode;
    std::vector<Storage::TextNote>& out;
    // One text. len: its length in bytes (a password in a file is stored as hex:
    // twice its length). now: what the clock keeps for it (Change mode: equal ->
    // not new input, passes). field: the member's size - what OnClock keeps.
    void text(const String& key, const String& what, const char* val, int limit, size_t field, Kind kind,
              const char* hint, const char* now = nullptr, size_t len = SIZE_MAX) {
        if (!val) return;
        if (len == SIZE_MAX) len = strlen(val);
        if ((int)len <= limit) return;
        if (mode == Storage::TextMode::Change && now && strcmp(val, now) == 0) return;
        String t = Texts::fill(Texts::tooLong, kind != Kind::Secret ? what + " \"" + shown(val) + "\"" : what, limitWords(limit, kind));
        if (mode == Storage::TextMode::OnClock) {
            if (kind != Kind::Secret && len > field - 1) {
                char kept[129];
                StringTools::utf8Cut(kept, sizeof(kept), val, std::min(field - 1, sizeof(kept) - 1));
                t += Texts::fill(Texts::usesMeanwhile, String(kept));
            }
            if (hint) t += String(" ") + hint;
        }
        out.push_back({key, t});
    }
};

const char* HINT_WIFI  = "Enter a shorter one and press Save Wi-Fi.";
const char* HINT_PLACE = "Choose the place again (Change place).";
const char* HINT_FILE  = "Import settings with a valid one.";
}  // namespace

String Storage::apPasswordProblem(const char* pw) {
    if (!pw) pw = "";
    size_t n = strlen(pw);
    if (!StringTools::asciiPlain(pw)) return Texts::apPwPlain;
    if ((int)n < AP_PASSWORD_MIN_CHARS) return Texts::fill(Texts::atLeastChars, String(AP_PASSWORD_MIN_CHARS));
    if ((int)n > AP_PASSWORD_MAX_CHARS) return Texts::fill(Texts::atMostChars, String(AP_PASSWORD_MAX_CHARS));
    return "";
}

std::vector<Storage::TextNote> Storage::configTextProblems(JsonObjectConst doc, const AppConfig* cur, TextMode mode) {
    std::vector<TextNote> out;
    if (doc.isNull()) return out;
    Checker c{mode, out};
    if (doc["projectName"].is<const char*>()) {   // kept as letters and digits only
        String n = StringTools::normalize(doc["projectName"].as<const char*>(), PROJECT_NAME_DEFAULT);
        c.text("projectName", Texts::labelClockName, n.c_str(), PROJECT_NAME_MAX_CHARS, sizeof(AppConfig::projectName), Kind::Letters,
               HINT_WIFI, cur ? cur->projectName : nullptr);
    }
    // Wi-Fi names and passwords: plain characters only (StringTools::asciiPlain, the user 2026-11-21) -
    // typed or imported: refused; already in the clock's files (an older version took them): kept, said.
    auto notPlain = [&](const String& key, const String& label, const char* v, const char* now) {
        if (!v || StringTools::asciiPlain(v)) return;
        if (now && strcmp(v, now) == 0 && mode == TextMode::Change) return;   // unchanged: not new input
        String t = Texts::fill(Texts::wifiNotPlain, label) + ".";
        if (mode == TextMode::OnClock) t += String(" ") + HINT_WIFI;
        out.push_back({key, t});
    };
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        c.text(wifiConfigKey("wifiSSID", i), Texts::fill(Texts::labelNetName, String(i + 1)), doc[wifiConfigKey("wifiSSID", i)], WIFI_SSID_MAX_BYTES,
               sizeof(AppConfig::wifiSSID), Kind::Plain, HINT_WIFI, cur ? wifiSsidAt(*cur, i) : nullptr);
        notPlain(wifiConfigKey("wifiSSID", i), Texts::fill(Texts::labelNetName, String(i + 1)), doc[wifiConfigKey("wifiSSID", i)],
                 cur ? wifiSsidAt(*cur, i) : nullptr);
        // A password: in a file, encrypted (hex, two digits a character); in a
        // change, as typed ("********" = unchanged).
        const char* p = doc[wifiConfigKey("wifiPassword", i)];
        if (p && strcmp(p, "********") != 0 && mode == TextMode::Change)   // as typed (in a file: encrypted)
            notPlain(wifiConfigKey("wifiPassword", i), Texts::fill(Texts::labelNetPassword, String(i + 1)), p, nullptr);
        if (p && strcmp(p, "********") != 0)
            c.text(wifiConfigKey("wifiPassword", i), Texts::fill(Texts::labelNetPassword, String(i + 1)), p, WIFI_PASSWORD_MAX_CHARS, sizeof(AppConfig::wifiPassword),
                   Kind::Secret, "Enter it again and press Save Wi-Fi.", nullptr, mode == TextMode::Change ? strlen(p) : strlen(p) / 2);
    }
    c.text("separator", Texts::labelSeparator, doc["separator"], SEPARATOR_MAX_BYTES, sizeof(AppConfig::messageSeparator), Kind::Hebrew,
           HINT_FILE, cur ? cur->messageSeparator : nullptr);
    // Texts without a limit of their own: what their field holds.
    c.text("country",  "Country",   doc["country"],  sizeof(AppConfig::locationCountry) - 1, sizeof(AppConfig::locationCountry), Kind::Hebrew, HINT_PLACE);
    c.text("city",     "City",      doc["city"],     sizeof(AppConfig::locationCity) - 1,    sizeof(AppConfig::locationCity),    Kind::Hebrew, HINT_PLACE);
    c.text("tzid",     "Time zone", doc["tzid"],     sizeof(AppConfig::tzid) - 1,            sizeof(AppConfig::tzid),            Kind::Plain,  HINT_PLACE);
    c.text("tzPOSIX",  "Time zone rule", doc["tzPOSIX"], sizeof(AppConfig::tzPOSIX) - 1,     sizeof(AppConfig::tzPOSIX),         Kind::Plain,  HINT_PLACE);
    c.text("language", "Language",  doc["language"], sizeof(AppConfig::language) - 1,        sizeof(AppConfig::language),        Kind::Plain,  HINT_FILE);
    c.text("logLevel", "Log level", doc["logLevel"], sizeof(AppConfig::logLevel) - 1,        sizeof(AppConfig::logLevel),        Kind::Plain,  HINT_FILE);
    c.text("pwd",      "Page password", doc["pwd"],  sizeof(AppConfig::webPassword) - 1,     sizeof(AppConfig::webPassword),     Kind::Secret, HINT_FILE);
    c.text("apPassword", "The clock's own Wi-Fi password", doc["apPassword"], AP_PASSWORD_MAX_CHARS, sizeof(AppConfig::apPassword), Kind::Secret, nullptr);
    c.text("updateFeed", "Update feed", doc["updateFeed"], sizeof(AppConfig::updateFeed) - 1, sizeof(AppConfig::updateFeed), Kind::Plain, HINT_FILE);
    c.text("updateKey", "Update key", doc["updateKey"], sizeof(AppConfig::updateKey) - 1, sizeof(AppConfig::updateKey), Kind::Plain, nullptr);
    c.text("updateKeyBy", "Update key's version", doc["updateKeyBy"], sizeof(AppConfig::updateKeyBy) - 1, sizeof(AppConfig::updateKeyBy), Kind::Plain, nullptr);
    JsonArrayConst places = doc["places"];
    for (int i = 0; places && i < WIFI_NETWORKS && i < (int)places.size(); i++) {
        JsonObjectConst o = places[i];
        String key = "places." + String(i + 1), net = "Network " + String(i + 1) + " place: ";
        c.text(key, net + "city",    o["city"],    sizeof(Place::city) - 1,    sizeof(Place::city),    Kind::Hebrew, HINT_PLACE);
        c.text(key, net + "country", o["country"], sizeof(Place::country) - 1, sizeof(Place::country), Kind::Hebrew, HINT_PLACE);
        c.text(key, net + "time zone", o["tzid"],  sizeof(Place::tzid) - 1,    sizeof(Place::tzid),    Kind::Plain,  HINT_PLACE);
        c.text(key, net + "time zone rule", o["tzPOSIX"], sizeof(Place::tzPOSIX) - 1, sizeof(Place::tzPOSIX), Kind::Plain, HINT_PLACE);
    }
    return out;
}

void Storage::eventProblems(JsonObjectConst ev, int number, TextMode mode, std::vector<TextNote>& out) {
    Checker c{mode, out};
    const char* name = ev["name"] | "";
    String key = "event:" + String(ev["id"] | "");
    String who = number ? Texts::fill(Texts::labelEvent, String(number)) : String(Texts::labelEventAlone);
    const char* hint = Texts::openToCorrect;
    c.text(key, Texts::fill(Texts::labelEventName, who), name, EVENT_NAME_MAX_BYTES, sizeof(PersonalEvent::name), Kind::Hebrew, hint);
    who += " (" + shown(name) + ")";
    c.text(key, who + " icon",    ev["icon"],      sizeof(PersonalEvent::icon) - 1,      sizeof(PersonalEvent::icon),      Kind::Plain, hint);
    c.text(key, who + " type",    ev["type"],      sizeof(PersonalEvent::type) - 1,      sizeof(PersonalEvent::type),      Kind::Plain, hint);
    c.text(key, who + " id",      ev["id"],        sizeof(PersonalEvent::id) - 1,        sizeof(PersonalEvent::id),        Kind::Plain, nullptr);
    c.text(key, who + " picture", ev["image"],     sizeof(PersonalEvent::image) - 1,     sizeof(PersonalEvent::image),     Kind::Plain, nullptr);
    c.text(key, who + " own icon", ev["iconImage"], sizeof(PersonalEvent::iconImage) - 1, sizeof(PersonalEvent::iconImage), Kind::Plain, nullptr);
    // Its dates (2026-11-14): every way in (typed, imported, restored, the
    // clock's own file) - DateRules says what a date may be, Config.h LIMITS
    // the years. In the clock's own file: the date is dropped when read
    // (eventFromJson()), the event then incomplete until it's fixed.
    auto say = [&](const String& what, const char side, const char* fields) {
        String t = who + ": " + what + ".";
        if (mode == TextMode::OnClock) t += String(" ") + hint;
        String f;   // "d y" -> "gD gY"
        for (const char* c = fields; *c; c++)
            if (*c != ' ') f += String(f.length() ? " " : "") + side + StringTools::asciiToUpper(*c);
        out.push_back({key, t, f, what});
    };
    EventDate d = eventDateOf(ev);
    const char* fields = "";
    if (d.gY && !inRange(EVENT_GREG_YEAR_RANGE, d.gY))
        say(Texts::fill(Texts::gYearOut, String(d.gY), Texts::fill(Texts::yearFrom, String(EVENT_GREG_YEAR_RANGE.lo), String(EVENT_GREG_YEAR_RANGE.hi))), 'g', "y");
    else if (d.gD || d.gM) {
        String p = DateRules::gregorianProblem(d.gY, d.gM, d.gD, &fields);
        if (p.length()) say(p, 'g', fields);
    }
    if (d.jY && !inRange(EVENT_HEB_YEAR_RANGE, d.jY))
        say(Texts::fill(Texts::hYearOut, String(d.jY), Texts::fill(Texts::yearFrom, String(EVENT_HEB_YEAR_RANGE.lo), String(EVENT_HEB_YEAR_RANGE.hi))), 'j', "y");
    else if (d.jD || d.jM) {
        String p = DateRules::hebrewProblem(d.jY, d.jM, d.jD, &fields);
        if (p.length()) say(p, 'j', fields);
        else if (d.jD && d.jM) {
            // A real date the anniversary rules still can't place on a day (never expected:
            // the user, 2026-11-14 - if it happens, the user hears it now, not the log):
            // tried in a complete, a deficient and a leap year (Cheshvan / Kislev 30 or 29,
            // one Adar or two).
            const bool yahrzeit = strcmp(ev["type"] | "", "yahrzeit") == 0;
            for (int y : {5785, 5786, 5787}) {
                HebrewDate o = {d.jY, d.jM, d.jD};
                HebrewDate r = yahrzeit ? JewishDateEngine::resolveYahrzeit(y, o) : JewishDateEngine::resolveBirthdayOrAnniversary(y, o);
                if (r.month == 0) { say(Texts::fill(Texts::noRuleDay, String(y)), 'j', "d m"); break; }
            }
        }
    }
    if (d.gD && d.gM && !(d.gY && !inRange(EVENT_GREG_YEAR_RANGE, d.gY)) && DateRules::gregorianProblem(d.gY, d.gM, d.gD).length() == 0)
        for (int y : {2027, 2028})   // a common and a leap year
            if (DateRules::resolveGregorian(y, d.gM, d.gD) == DateRules::RESOLVE_NONE) {
                say("the clock's date rules found no day for it in " + String(y) + " - please report this", 'g', "d m");
                break;
            }
}

String Storage::joinTexts(const std::vector<TextNote>& list) {
    String s;
    for (const TextNote& n : list) s += (s.length() ? "\n" : "") + n.text;
    return s;
}

String Storage::checkTexts(JsonObjectConst doc, const AppConfig& cur) {
    return joinTexts(configTextProblems(doc, &cur, TextMode::Change));
}

// The notes from the clock's own files (OnClock): written by the loads (main
// task at start-up), read and cleared by web handlers (core 0) - so under a lock.
static std::mutex s_notesLock;
static std::vector<Storage::TextNote> s_notes;
static bool writeJsonAtomic(const char* path, const JsonDocument& doc);   // below

// The notes are kept in TEXT_NOTES_PATH too (2026-11-15): the next save writes the shortened
// text back, so after a restart the file no longer shows it was cut - found when renaming the
// clock started to restart it. A note now stays until that text is changed (clearTextNote),
// the settings or events are restored (forgetTextNotes) or the clock is reset (a clock data path).
// Said on the clock's screen too (the user, 2026-11-15: found without anyone doing anything, the
// clock says it - "Error - see Settings" on the status slide), for
// TEXT_NOTES_SCREEN_DAYS from the day it first showed it; a new problem starts them again.
// Settings lists them until each text is changed; "OK, seen" there takes them off the screen (the
// user, 2026-11-15: each problem on its own - a new one is unseen again). The file: {"day": <that
// local day, 0 = not shown yet>, "seen": bool, "notes": [{key, text}...]} (a plain list until
// 2026-11-15: still read).
static const long TEXT_NOTES_SCREEN_DAYS = 3;   // === TUNABLE: days the screen says it
TUNABLE_SHARED(TEXT_NOTES_SCREEN_DAYS)
static long s_notesDay = 0;
static bool s_notesSeen = false;

struct NotesFile { long day = 0; bool seen = false; std::vector<Storage::TextNote> notes; };
static NotesFile readNotesFile();
static bool sameNotes(const std::vector<Storage::TextNote>& a, const std::vector<Storage::TextNote>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (a[i].key != b[i].key || a[i].text != b[i].text) return false;
    return true;
}
static void writeNotesDoc(const std::vector<Storage::TextNote>& notes, long day, bool seen) {
    JsonDocument doc;
    doc["day"] = day;
    doc["seen"] = seen;
    JsonArray a = doc["notes"].to<JsonArray>();
    for (const auto& n : notes) { JsonObject o = a.add<JsonObject>(); o["key"] = n.key; o["text"] = n.text; }
    writeJsonAtomic(TEXT_NOTES_PATH, doc);
}
static void writeNotesFile(const std::vector<Storage::TextNote>& notes) {   // s_notesLock held
    if (notes.empty()) { s_notesDay = 0; s_notesSeen = false; if (LittleFS.exists(TEXT_NOTES_PATH)) LittleFS.remove(TEXT_NOTES_PATH); return; }
    NotesFile now = readNotesFile();
    // A note not there before: a new problem - the screen says it for its days again, unseen
    bool grew = false;
    for (const auto& n : notes) {
        bool had = false;
        for (const auto& o : now.notes) had |= o.key == n.key && o.text == n.text;
        grew |= !had;
    }
    long day = grew ? 0 : (s_notesDay ? s_notesDay : now.day);
    bool seen = grew ? false : (s_notesSeen || now.seen);
    s_notesDay = day;
    s_notesSeen = seen;
    if (sameNotes(now.notes, notes) && day == now.day && seen == now.seen) return;   // the same already: no write (every start loads them)
    writeNotesDoc(notes, day, seen);
}

static NotesFile readNotesFile() {   // a small file of our own: read at start and on a change
    NotesFile out;
    File f = LittleFS.open(TEXT_NOTES_PATH, "r");
    if (!f) return out;
    JsonDocument doc;
    bool bad = deserializeJson(doc, f) != DeserializationError::Ok;
    f.close();
    if (bad) return out;   // damaged: the loads say again what they find
    JsonArrayConst list = doc.is<JsonArray>() ? doc.as<JsonArrayConst>() : doc["notes"].as<JsonArrayConst>();
    if (doc.is<JsonObject>()) { out.day = doc["day"] | 0L; out.seen = doc["seen"] | false; }
    for (JsonObjectConst o : list) {
        const char* k = o["key"] | "";
        const char* t = o["text"] | "";
        if (k[0] && t[0] && strlen(k) < 80 && strlen(t) < 400) out.notes.push_back({k, t});
    }
    return out;
}

bool Storage::textNotesOnScreen(long today) {
    std::lock_guard<std::mutex> g(s_notesLock);
    if (s_notes.empty() || s_notesSeen) return false;
    if (today <= 0) return true;   // the date not known yet: say it
    if (!s_notesDay) {   // first on the screen today: the days count from here (written once)
        NotesFile inFile = readNotesFile();
        s_notesDay = inFile.day ? inFile.day : today;
        if (!inFile.day) writeNotesDoc(s_notes, s_notesDay, s_notesSeen);
    }
    return today - s_notesDay < TEXT_NOTES_SCREEN_DAYS;
}

bool Storage::textNotesSeen() {
    std::lock_guard<std::mutex> g(s_notesLock);
    return s_notesSeen;
}

void Storage::textNotesSeenNow() {
    std::lock_guard<std::mutex> g(s_notesLock);
    if (s_notes.empty() || s_notesSeen) return;
    s_notesSeen = true;
    writeNotesDoc(s_notes, s_notesDay, true);
    DiagnosticLog::record("Storage", "texts too long: 'OK, seen' in Settings - off the clock's screen");
}

static void replaceNotes(bool events, std::vector<Storage::TextNote> fresh) {
    std::lock_guard<std::mutex> g(s_notesLock);
    auto kind = [events](const Storage::TextNote& n) { return n.key.startsWith("event:") == events; };
    s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(), kind), s_notes.end());
    // Kept from before (this kind, a key the load found nothing for now), then what it found
    const NotesFile before = readNotesFile();
    for (const auto& n : before.notes) {
        if (!kind(n)) continue;
        bool found = false;
        for (const auto& f : fresh) found |= f.key == n.key;
        if (!found) s_notes.push_back(n);
    }
    for (auto& n : fresh) s_notes.push_back(n);
    for (auto& n : fresh) DiagnosticLog::record("Storage", "Too long in a file: %s", n.text.c_str());
    // Both kinds in the file: the other kind's notes come from it as they were
    std::vector<Storage::TextNote> all = s_notes;
    for (const auto& n : before.notes) {
        if (kind(n)) continue;
        bool have = false;
        for (const auto& a : all) have |= a.key == n.key && a.text == n.text;
        if (!have) all.push_back(n);
    }
    writeNotesFile(all);
}
std::vector<Storage::TextNote> Storage::textNotes() {
    std::lock_guard<std::mutex> g(s_notesLock);
    return s_notes;
}
void Storage::clearTextNote(const String& key) {
    std::lock_guard<std::mutex> g(s_notesLock);
    size_t before = s_notes.size();
    s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(), [&](const TextNote& n) { return n.key == key; }), s_notes.end());
    if (s_notes.size() != before) writeNotesFile(s_notes);
}
void Storage::clearTextNotes(const String& prefix) {
    std::lock_guard<std::mutex> g(s_notesLock);
    size_t before = s_notes.size();
    s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(), [&](const TextNote& n) { return n.key.startsWith(prefix); }), s_notes.end());
    if (s_notes.size() != before) writeNotesFile(s_notes);
}
void Storage::forgetTextNotes(bool config, bool events) {
    std::lock_guard<std::mutex> g(s_notesLock);
    s_notes.erase(std::remove_if(s_notes.begin(), s_notes.end(), [&](const TextNote& n) {
        return n.key.startsWith("event:") ? events : config; }), s_notes.end());
    writeNotesFile(s_notes);
}

// Replaces `path` only with content that is known to be complete: written to
// "<path>.tmp", its size on flash checked against what was serialized, then
// renamed over the real file (LittleFS rename replaces the target atomically). Until that rename
// the previous file is untouched, so whatever goes wrong mid-save - a reset,
// a power cut, or a serialize that produced nothing - the clock keeps its
// last good settings/events. On 2026-10-16 config.json was found EMPTY after
// a restart (every setting reverted to defaults); the exact mechanism wasn't
// pinned down, so failures here are also logged to make a repeat traceable.
static bool writeJsonAtomic(const char* path, const JsonDocument& doc) {
    return FileTools::writeJsonAtomic(path, doc);   // whole or not at all - see FileTools.h
}

// =============================================================================
// saveConfig()
// =============================================================================

// Uptime::ms() of the oldest change waiting for saveConfigSoon()'s save, 0 = none.
static std::atomic<int64_t> s_saveSoonSinceMs{0};

void Storage::saveConfigSoon() {
    int64_t none = 0;
    s_saveSoonSinceMs.compare_exchange_strong(none, Uptime::ms() | 1);
}

void Storage::loop(const AppConfig& cfg) {
    int64_t since = s_saveSoonSinceMs;
    if (since && DisplayManager::goodMomentToWrite(since)) saveConfig(cfg);
}

bool Storage::saveConfig(const AppConfig& cfg) {
    s_saveSoonSinceMs = 0;   // this save covers a waiting one
    JsonDocument doc;

    // Saved under "wifiPassword" (2, 3), but always in its encrypted form - the
    // plaintext only ever exists in RAM (see Storage.h, WI-FI PASSWORD SECURITY).
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        doc[wifiConfigKey("wifiSSID", i)]     = wifiSsidAt(cfg, i);
        doc[wifiConfigKey("wifiPassword", i)] = encryptPassword(wifiPasswordAt(cfg, i));
    }
    doc["wifiLastNetwork"] = cfg.wifiLastNetwork;

    doc["projectName"] = cfg.projectName;

    doc["country"]  = cfg.locationCountry;
    doc["city"]     = cfg.locationCity;
    doc["lat"]      = cfg.latitude;
    doc["lon"]      = cfg.longitude;
    doc["tzid"]     = cfg.tzid;
    doc["tzPOSIX"]  = cfg.tzPOSIX;
    doc["rolloverMode"] = static_cast<int>(cfg.rolloverMode);
    doc["inIsrael"] = cfg.inIsrael;
    JsonArray placesArr = doc["places"].to<JsonArray>();
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        const Place& p = cfg.places[i];
        JsonObject o = placesArr.add<JsonObject>();
        o["set"] = p.set; o["confirmed"] = p.confirmed;
        o["city"] = p.city; o["country"] = p.country; o["countryCode"] = p.countryCode;
        o["lat"] = p.lat; o["lon"] = p.lon;
        o["tzid"] = p.tzid; o["tzPOSIX"] = p.tzPOSIX; o["offsetSec"] = p.offsetSec;
    }
    doc["use24Hour"]= cfg.use24Hour;
    doc["autoUpdate"] = cfg.autoUpdate;
    if (cfg.updateFeed[0]) doc["updateFeed"] = cfg.updateFeed;
    doc["updateKey"] = cfg.updateKey;
    doc["updateKeyBy"] = cfg.updateKeyBy;

    JsonArray ntpArr = doc["ntpServers"].to<JsonArray>();
    for (int i = 0; i < NTP_SERVER_SLOTS; i++)   // all of them, empty ones too (see load)
        ntpArr.add(cfg.ntpServers[i]);

    doc["language"] = cfg.language;

    doc["debugTimeOffset"] = cfg.debugTimeOffset;
    doc["rotationMode"]    = static_cast<int>(cfg.rotationMode);
    doc["messageDwellSec"] = cfg.messageDwellSec;
    doc["separator"]       = cfg.messageSeparator;
    doc["scrollSpeed"]     = cfg.scrollSpeed;
    doc["fontCharGapPx"]   = cfg.fontCharGapPx;
    doc["fontWordGapPx"]   = cfg.fontWordGapPx;
    doc["fontWidthScalePct"] = cfg.fontWidthScalePct;

    doc["brightPct"]        = cfg.brightPct;
    doc["useLightSensor"]   = cfg.useLightSensor;
    doc["nightPct"]         = cfg.nightPct;
    doc["dayPct"]           = cfg.dayPct;
    doc["roomDarkPct"]      = roundf(cfg.roomDarkPct * 10) / 10;
    doc["roomLightPct"]     = roundf(cfg.roomLightPct * 10) / 10;
    doc["learnRoomLight"]   = cfg.learnRoomLight;

    doc["showHebrewYear"]    = cfg.showHebrewYear;
    doc["showShabbatShalom"] = cfg.showShabbatShalom;
    doc["calibX"] = cfg.calibX;
    doc["calibY"] = cfg.calibY;
    doc["calibW"] = cfg.calibW;
    doc["calibH"] = cfg.calibH;
    doc["calibTearUs"] = cfg.calibTearUs;

    doc["showRoshChodesh"]    = cfg.showRoshChodesh;
    doc["showFasts"]          = cfg.showFasts;
    doc["omerMode"]           = static_cast<int>(cfg.omerMode);
    doc["showSpecialShabbat"] = cfg.showSpecialShabbat;
    doc["showLiturgical"]     = cfg.showLiturgical;
    doc["showSwitchover"]     = cfg.showSwitchoverPhrases;
    doc["prayerSwitchDays"]   = cfg.switchoverWindowDays;
    doc["nusach"]             = static_cast<int>(cfg.nusach);
    doc["eventDayChange"]     = static_cast<int>(cfg.eventDayChange);
    writeShowWindows(doc.as<JsonObject>(), cfg);

    JsonArray orderArr = doc["contentOrder"].to<JsonArray>();
    for (int i = 0; i < (int)ContentType::COUNT; i++) orderArr.add(CONTENT_TYPE_KEYS[cfg.contentOrder[i]]);
    JsonObject freqObj = doc["contentFrequency"].to<JsonObject>();
    for (int t = 0; t < (int)ContentType::COUNT; t++) freqObj[CONTENT_TYPE_KEYS[t]] = cfg.contentFrequency[t];

    doc["pwdEnabled"]    = cfg.webPasswordEnabled;
    doc["pwd"]           = cfg.webPassword;
    doc["setupComplete"] = cfg.setupComplete;

    doc["logLevel"] = cfg.logLevel;
    doc["logLevelUntil"] = cfg.logLevelUntil;
    if (cfg.apPassword[0]) doc["apPassword"] = cfg.apPassword;

    if (!writeJsonAtomic(FS_CONFIG_PATH, doc)) return false;
    CLOCK_LOG("Storage", "Config saved");
    return true;
}

// =============================================================================
// loadEvents() / saveEvents()
// =============================================================================

// See PersonalEvent::id's comment (Config.h) for why a 64-bit hardware-RNG
// value rather than a persisted counter: a counter's uniqueness guarantee
// only holds as long as its own saved state is never lost or rolled back,
// and this project has already shown real ways that happens (stale config
// restores, uploadfs). A random id needs no memory of what came before, so
// none of those scenarios can produce a collision - the existence check
// below is just a (practically never-taken) belt-and-suspenders loop, not
// what actually makes this safe.
String Storage::generateEventId(const EventList& existing) {
    while (true) {
        uint64_t candidate = ((uint64_t)esp_random() << 32) | esp_random();
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)candidate);
        bool collision = false;
        for (const auto& ev : existing) {
            if (strcmp(ev.id, buf) == 0) { collision = true; break; }
        }
        if (!collision) return String(buf);
    }
}

// Used by loadEvents(). There's exactly one real
// events.json for this device, already on the current schema (id, jM as an
// int, jYNum as an int) - no older schema can ever reappear, so this reads
// it directly with no backward-compatibility branching.
// ─── Events <-> JSON: the one mapping (2026-11-14) ───────────────────────────
// Was written four times (load, save, the page's list, the page's save/import);
// only the page's way checked the type, the icon and the picture ids - a file
// edited by hand could name any path as a picture.
static const Storage::EventType EVENT_TYPES[] = {
    { "birthday", "🎂" }, { "wedding", "💍" }, { "yahrzeit", "🕯️" }, { "other", "🗓️" },
};
const Storage::EventType* Storage::eventTypes(size_t& count) {
    count = sizeof(EVENT_TYPES) / sizeof(EVENT_TYPES[0]);
    return EVENT_TYPES;
}
String Storage::normalizeEventType(const String& raw) {
    String t = raw;
    t.trim();
    t.toLowerCase();
    for (const auto& et : EVENT_TYPES) if (t == et.type) return t;
    return "";
}
const char* Storage::defaultIconForType(const char* type) {
    for (const auto& et : EVENT_TYPES) if (!strcmp(type, et.type)) return et.icon;
    return EVENT_TYPES[sizeof(EVENT_TYPES) / sizeof(EVENT_TYPES[0]) - 1].icon;   // unknown: "other"'s
}

// An event's date numbers as given - both shapes: the file's "jM" (1 = Tishrei
// ... 13 = Adar II), or the page's month name plus isAdarII. Shared by
// eventProblems() (what to say) and eventFromJson() (what to keep).
Storage::EventDate Storage::eventDateOf(JsonObjectConst in) {
    EventDate d;
    d.gD = in["gD"] | 0; d.gM = in["gM"] | 0; d.gY = in["gY"] | 0;
    d.jD = in["jD"] | 0;
    if (in["jM"].is<int>()) d.jM = in["jM"].as<int>();
    else if (in["jMonth"].is<const char*>()) {
        int m = JewishDateEngine::getMonthFromName(in["jMonth"] | "");
        d.jM = (m == HebrewMonths::ADAR && (in["isAdarII"] | false)) ? HebrewMonths::ADAR_II : m;
    }
    d.jY = in["jYNum"].is<int>() ? in["jYNum"].as<int>() : (in["jY"] | 0);
    return d;
}

void Storage::eventFromJson(JsonObjectConst in, PersonalEvent& e, EventJson shape) {
    e = PersonalEvent{};
    StringTools::utf8Copy(e.id, in["id"] | "");
    StringTools::utf8Copy(e.name, in["name"] | "");   // too long: eventProblems() first
    // Only real dates are kept (DateRules; eventProblems() said what's wrong
    // first): a year outside LIMITS -> no year; a date no year has, or not in
    // its year -> no date (the event then incomplete - the page flags it)
    // instead of feeding date arithmetic nonsense. Only from a file edited by
    // hand, or a client other than the page - those are refused before this.
    EventDate d = eventDateOf(in);
    (void)shape;   // both shapes read by eventDateOf()
    if (!inRange(EVENT_GREG_YEAR_RANGE, d.gY)) d.gY = 0;
    if (!inRange(EVENT_HEB_YEAR_RANGE, d.jY))  d.jY = 0;
    if (!DateRules::validGregorian(d.gY, d.gM, d.gD)) { d.gD = 0; if (d.gM < 0 || d.gM > 12) d.gM = 0; }
    if (!DateRules::validHebrew(d.jY, d.jM, d.jD))    { d.jD = 0; if (d.jM < 0 || d.jM > 13) d.jM = 0; }
    e.hasGregorian = in["hasG"] | false;
    e.gDay   = d.gD; e.gMonth = d.gM; e.gYear = d.gY;
    e.afterSunset  = in["afterSunset"] | false;
    e.hasJewish    = in["hasJ"] | false;
    e.jDay   = d.jD; e.jMonth = d.jM; e.jYear = d.jY;
    StringTools::utf8Copy(e.type, normalizeEventType(in["type"] | "birthday").c_str());
    StringTools::utf8Copy(e.icon, in["icon"] | "");
    if (!e.icon[0]) StringTools::utf8Copy(e.icon, defaultIconForType(e.type));
    const char* image = in["image"] | "";          // a picture from the library, or none
    if (ImageLibrary::isValidId(image)) StringTools::utf8Copy(e.image, image);
    const char* iconImage = in["iconImage"] | "";  // the icon as a picture, or none
    if (ImageLibrary::isValidId(iconImage)) StringTools::utf8Copy(e.iconImage, iconImage);
    e.warningDays = clampTo(EVENT_WARN_DAYS_RANGE, in["warn"] | 0);   // Config.h LIMITS
}

void Storage::eventToJson(const PersonalEvent& e, JsonObject out, EventJson shape) {
    out["id"]          = e.id;
    out["name"]        = e.name;
    out["hasG"]        = e.hasGregorian;
    out["gD"]          = e.gDay;
    out["gM"]          = e.gMonth;
    out["gY"]          = e.gYear;
    out["afterSunset"] = e.afterSunset;
    out["hasJ"]        = e.hasJewish;
    out["jD"]          = e.jDay;
    if (shape == EventJson::File) {
        out["jM"] = e.jMonth;
    } else {
        out["jMonth"]   = (e.jMonth == 13) ? "אדר" : JewishDateEngine::getMonthName(e.jMonth);
        out["isAdarII"] = (e.jMonth == 13);
    }
    out["jYNum"] = e.jYear;
    char yHeb[32] = "";   // the year as written, for people reading the file - none without a year
    // (2026-11-18: numToGematria(0) says "?" since 2026-11-13 - every yearless Hebrew event read
    // "17 אדר ?" in Settings' search and in events.json)
    if (e.jYear >= 5000)  JewishDateEngine::yearToGematria(e.jYear, yHeb, sizeof(yHeb));
    else if (e.jYear > 0) JewishDateEngine::numToGematria(e.jYear, yHeb, sizeof(yHeb));
    out["jY"]    = yHeb;
    out["icon"]  = e.icon;
    out["warn"]  = e.warningDays;
    out["type"]  = e.type[0] ? e.type : "birthday";
    out["image"] = e.image;                             // ImageLibrary id, "" = none
    if (e.iconImage[0]) out["iconImage"] = e.iconImage;   // only when there is one: older files stay as they were
}

// onClock: the clock's own events.json - texts too long are noted for the page.
static void parseEventsFromDoc(JsonDocument& doc, EventList& events, bool onClock) {
    JsonArray arr = doc.as<JsonArray>();
    int count = min((int)arr.size(), MAX_EVENTS);
    events.clear();
    if (onClock) {
        std::vector<Storage::TextNote> notes;
        for (int i = 0; i < count; i++) Storage::eventProblems(arr[i].as<JsonObjectConst>(), i + 1, Storage::TextMode::OnClock, notes);
        // More than it holds (edited by hand, another version): kept as far as it goes, said in
        // Settings like a text cut short - never dropped silently (2026-11-20)
        if ((int)arr.size() > MAX_EVENTS)
            notes.push_back({"event:count", Texts::fill(Texts::eventsFileTooMany, String((int)arr.size()), String(MAX_EVENTS), String(MAX_EVENTS))});
        replaceNotes(true, notes);
    }
    // PsramAllocator (PsramVector.h) throws std::bad_alloc if PSRAM is too
    // full/fragmented to grow the vector - unlike the Web UI's upsert/import
    // handlers, there is no percentage-free pre-check at either of this
    // function's two call sites (boot-time loadEvents(), and /api/restore's
    // the restore before 2026-11-18), so this is the only guard they get. On failure,
    // keep whatever was already parsed rather than losing the whole list.
    try {
        events.reserve(count);
        for (int i = 0; i < count; i++) {
            PersonalEvent e;
            Storage::eventFromJson(arr[i].as<JsonObjectConst>(), e, Storage::EventJson::File);
            events.push_back(e);
        }
    } catch (const std::bad_alloc&) {
        DiagnosticLog::record("Storage", "Out of memory loading events - kept %d of %d", (int)events.size(), count);
    }
}

bool Storage::loadEvents(EventList& events, const char* path) {
    events.clear();
    if (!exists(path)) return true;

    File f = LittleFS.open(path, "r");
    if (!f) return false;

    FileTools::skipUtf8ByteOrderMark(f);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        DiagnosticLog::record("Storage", "%s unreadable (%s) - no events from it until it's fixed", path, err.c_str());
        return false;
    }

    parseEventsFromDoc(doc, events, strcmp(path, FS_EVENTS_PATH) == 0);
    CLOCK_LOG("Storage", "Loaded %d personal events from %s", (int)events.size(), path);
    return true;
}

bool Storage::saveEvents(const EventList& events, const char* path) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& e : events) eventToJson(e, arr.add<JsonObject>(), EventJson::File);

    if (!writeJsonAtomic(path, doc)) return false;
    CLOCK_LOG("Storage", "Saved %d personal events to %s", (int)events.size(), path);
    return true;
}

// =============================================================================
// Bundled backup / restore
// =============================================================================
//
// Backup format:
//   { "version": "1.1.0", "config": { ... }, "events": [ ... ] }
//
// The backup's "config" is config.json as stored, so the Wi-Fi password is in
// it in encrypted form (key "wifiPassword") - restoring a backup in AP mode
// immediately restores the Wi-Fi credentials.

String Storage::exportBackup() {
    // Build without re-parsing the files, to avoid large intermediate documents
    String cfg  = exportConfigBackup();
    String evts = exportEventsBackup();
    String out;
    out.reserve(cfg.length() + evts.length() + 64);
    out  = "{\"version\":\"";
    out += FIRMWARE_VERSION;
    out += "\",\"config\":";
    out += cfg;
    out += ",\"events\":";
    out += evts;
    out += "}";
    return out;
}

bool Storage::importBackup(const String& json, String& why) {
    // We parse just enough to extract the two sub-sections
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        LOG_AT(LogLevel::WARN, "Storage", "Restore-backup parse error - upload rejected");
        why = "Invalid backup format";
        return false;
    }
    // Made by a version this one can read? (VersionRule.h - the page passes an Excel file's on)
    why = VersionRule::dataProblem(FIRMWARE_VERSION, doc["version"] | "", "This backup");
    if (why.length()) {
        why = Texts::fill(Texts::restoreRefused, why);
        LOG_AT(LogLevel::WARN, "Storage", "restore refused: %s", why.c_str());
        return false;
    }
    // Settings only (the user, 2026-11-18): events come in through the Excel import alone - with
    // its row checks, Undo and pictures. This door replaced every event at once, used by nothing.
    if (!doc["events"].isNull()) {
        why = Texts::restoreSettingsOnly;
        return false;
    }
    // Every text against its limit before anything is written: all or nothing.
    std::vector<TextNote> bad = configTextProblems(doc["config"].as<JsonObjectConst>(), nullptr, TextMode::Restore);
    if (!bad.empty()) {
        why = Texts::fill(Texts::restoreTooLong, joinTexts(bad));
        return false;
    }
    why = "";

    bool ok = true;
    // What is replaced takes its notes with it (nothing in a restore is too long: refused above)
    forgetTextNotes(!doc["config"].isNull(), false);

    if (doc["config"]) {
        String cfgStr;
        serializeJson(doc["config"], cfgStr);
        ok &= importConfigBackup(cfgStr);
    }

    return ok;
}

// ─── Individual section backup ────────────────────────────────────────────────

String Storage::exportConfigBackup() {
    if (!exists(FS_CONFIG_PATH)) return "{}";
    File f = LittleFS.open(FS_CONFIG_PATH, "r");
    String s = f.readString();
    f.close();
    return s;
}

String Storage::exportEventsBackup() {
    if (!exists(FS_EVENTS_PATH)) return "[]";
    File f = LittleFS.open(FS_EVENTS_PATH, "r");
    String s = f.readString();
    f.close();
    return s;
}

bool Storage::importConfigBackup(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    return writeJsonAtomic(FS_CONFIG_PATH, doc);
}

// =============================================================================
// Utility
// =============================================================================

bool Storage::exists(const char* path) { return LittleFS.exists(path); }
bool Storage::remove(const char* path) { return LittleFS.remove(path); }
size_t Storage::freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }
