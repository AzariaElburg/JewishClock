// =============================================================================
// Storage.cpp
// JewishClock - LittleFS persistent storage implementation.
// =============================================================================

#include "Storage.h"
#include "Config.h"
#include "Version.h"
#include "StringTools.h"
#include "NTPManager.h"
#include "JewishDateEngine.h"
#include "FileTools.h"
#include "DiagnosticLog.h"
#include "Display.h"
#include "LogLevel.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <atomic>

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

void Storage::factoryReset() {
    DiagnosticLog::discardPending();   // lines waiting for the log file go with it
    std::vector<String> files, dirs;
    listAll("/", files, dirs);
    int removed = 0;
    for (const String& p : files) if (isClockDataPath(p) && LittleFS.remove(p)) removed++;
    for (const String& p : dirs)  if (isClockDataPath(p)) LittleFS.rmdir(p);
    AppConfig def;
    applyDefaults(def);
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
    strncpy(cfg.projectName, normalizedName.c_str(), sizeof(cfg.projectName));
    cfg.wifiSSID[0]     = '\0';
    cfg.wifiPassword[0] = '\0';
    cfg.wifiLastNetwork = -1;

    // Until the first network's place is known (Location.cpp): Jerusalem,
    // the same values loadConfig() falls back to.
    strncpy(cfg.locationCountry, "Israel",    sizeof(cfg.locationCountry));
    strncpy(cfg.locationCity,    "Jerusalem", sizeof(cfg.locationCity));
    cfg.latitude  = DEFAULT_LATITUDE;
    cfg.longitude = DEFAULT_LONGITUDE;
    strncpy(cfg.tzid, "Asia/Jerusalem", 64);
    strncpy(cfg.tzPOSIX, DEFAULT_TZ_POSIX, sizeof(cfg.tzPOSIX));
    cfg.inIsrael  = DEFAULT_IN_ISRAEL;
    cfg.rolloverMode = ROLLOVER_MODE_DEFAULT;
    memset(cfg.places, 0, sizeof(cfg.places));   // no network's place known yet

    cfg.use24Hour      = true;
    cfg.ntpServerCount = 3;
    strncpy(cfg.ntpServers[0], NTP_SERVER_1, 64);
    strncpy(cfg.ntpServers[1], NTP_SERVER_2, 64);
    strncpy(cfg.ntpServers[2], NTP_SERVER_3, 64);
    cfg.manualTimeSet  = false;

    strncpy(cfg.language, LANG_DEFAULT, sizeof(cfg.language));

    cfg.debugTimeOffset = 0;
    cfg.devMode         = false;
    cfg.rotationMode    = ROTATION_MODE_DEFAULT;
    cfg.messageDwellSec = MESSAGE_DWELL_SEC_DEFAULT;
    strncpy(cfg.messageSeparator, MESSAGE_SEPARATOR_DEFAULT, sizeof(cfg.messageSeparator));
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
    cfg.showAllMessages       = true;

    // Default order matches the historical hardcoded build order in
    // MessageAggregator; default frequencies match the old individual
    // show* defaults (1 = every round/on, 0 = off).
    static const ContentType kDefaultOrder[(int)ContentType::COUNT] = {
        ContentType::Events, ContentType::Candle, ContentType::Havdalah, ContentType::Holiday,
        ContentType::Mevarchim, ContentType::Parasha, ContentType::Omer, ContentType::DafYomi,
        ContentType::MishnaYomit, ContentType::HebrewDate, ContentType::GregorianDate,
        // Newest type LAST: a saved config's contentOrder overwrites slots
        // from the front, so a type the saved list doesn't know about yet is
        // only kept (appended at the end) if it's at the tail of this default.
        ContentType::Fast, ContentType::PrayerSwitch
    };
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

    strlcpy(cfg.logLevel, "info", sizeof(cfg.logLevel));
}

// =============================================================================
// loadConfig()
// =============================================================================

bool Storage::loadConfig(AppConfig& cfg) {
    applyDefaults(cfg);  // Always start from defaults, then overlay saved values

    if (!exists(FS_CONFIG_PATH)) {
        CLOCK_LOG("Storage", "No config file — using defaults");
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


    // Load the obfuscated passwords. If missing, WiFi.begin will fail
    // gracefully and trigger the AP portal where the user can re-enter it.
    for (int i = 0; i < WIFI_NETWORKS; i++) {
        strlcpy(wifiSsidAt(cfg, i), doc[wifiConfigKey("wifiSSID", i)] | "", sizeof(cfg.wifiSSID));
        const char* enc = doc[wifiConfigKey("wifiPassword", i)];
        if (enc) decryptPassword(enc, wifiPasswordAt(cfg, i), sizeof(cfg.wifiPassword));
        else     wifiPasswordAt(cfg, i)[0] = '\0';
    }
    cfg.wifiLastNetwork = doc["wifiLastNetwork"] | -1;
    const char* enc = doc["wifiPassword"];
    if (enc) {
        // Encryption here is for the password's trip over the air during
        // setup (see encryptPassword()'s own comment), not for keeping it
        // off the Serial console - so the encrypted form (useless without
        // the firmware's embedded key) stays visible at the normal level,
        // while the decrypted plaintext only prints at the top (Debug)
        // verbosity tier, when someone's deliberately looking for it.
        CLOCK_LOG("Storage", "Loaded Wi-Fi credentials for SSID: %s . Encrypted: %s", (const char*)cfg.wifiSSID, enc);
        if (Logging::enabled(LogLevel::DEBUG)) {
            CLOCK_LOG("Storage", "Decrypted Wi-Fi password: %s", (const char*)cfg.wifiPassword);
        }
    }

    String rawProjectName = doc["projectName"] | PROJECT_NAME_DEFAULT;
    strncpy(cfg.projectName, normalizeProjectName(rawProjectName).c_str(), sizeof(cfg.projectName));

    strlcpy(cfg.locationCountry, doc["country"] | "Israel",    sizeof(cfg.locationCountry));
    strlcpy(cfg.locationCity,    doc["city"]    | "Jerusalem", sizeof(cfg.locationCity));
    cfg.latitude  = doc["lat"]      | DEFAULT_LATITUDE;
    cfg.longitude = doc["lon"]      | DEFAULT_LONGITUDE;
    strlcpy(cfg.tzid, doc["tzid"] | "Asia/Jerusalem", 64);
    strlcpy(cfg.tzPOSIX, doc["tzPOSIX"] | DEFAULT_TZ_POSIX, 128);
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
        strlcpy(p.city,    o["city"]    | "", sizeof(p.city));
        strlcpy(p.country, o["country"] | "", sizeof(p.country));
        p.lat = o["lat"] | 0.0f;
        p.lon = o["lon"] | 0.0f;
        strlcpy(p.tzid,    o["tzid"]    | "", sizeof(p.tzid));
        strlcpy(p.tzPOSIX, o["tzPOSIX"] | "", sizeof(p.tzPOSIX));
        p.offsetSec = o["offsetSec"] | 0;
    }
    cfg.use24Hour = doc["use24Hour"] | true;

    cfg.debugTimeOffset = doc["debugTimeOffset"] | (int64_t)0;
    cfg.devMode         = doc["devMode"]         | false;
    NTPManager::setDebugOffset(cfg.debugTimeOffset);

    JsonArray ntpArr = doc["ntpServers"];
    if (ntpArr) {
        cfg.ntpServerCount = min((int)ntpArr.size(), 4);
        for (int i = 0; i < cfg.ntpServerCount; i++)
            strlcpy(cfg.ntpServers[i], ntpArr[i] | "", sizeof(cfg.ntpServers[i]));
    }

    strncpy(cfg.language, doc["language"] | LANG_DEFAULT, sizeof(cfg.language));

    cfg.rotationMode    = static_cast<RotationMode>(doc["rotationMode"] | static_cast<int>(ROTATION_MODE_DEFAULT));
    cfg.messageDwellSec = doc["messageDwellSec"] | MESSAGE_DWELL_SEC_DEFAULT;
    strncpy(cfg.messageSeparator, doc["separator"] | MESSAGE_SEPARATOR_DEFAULT, 16);
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
    cfg.switchoverWindowDays  = doc["prayerSwitchDays"]   | SWITCHOVER_DISPLAY_DAYS;   // new key (2026-10-17): the old "switchoverDays" was an unused setting whose saved 0 meant something else
    cfg.showAllMessages       = doc["showAll"]            | true;

    // Order is a list of type keys, frequency a key->int map. Either one
    // missing (or a type the saved lists don't mention) keeps the
    // applyDefaults() value.
    JsonArray orderArr = doc["contentOrder"];
    int i = 0;
    for (JsonVariant v : orderArr) {
        if (i >= (int)ContentType::COUNT) break;
        const char* key = v.as<const char*>();
        for (int t = 0; t < (int)ContentType::COUNT; t++) {
            if (key && strcmp(key, CONTENT_TYPE_KEYS[t]) == 0) { cfg.contentOrder[i] = (uint8_t)t; break; }
        }
        i++;
    }
    JsonObject freqObj = doc["contentFrequency"];
    for (int t = 0; t < (int)ContentType::COUNT; t++) {
        cfg.contentFrequency[t] = freqObj[CONTENT_TYPE_KEYS[t]] | cfg.contentFrequency[t];
    }
    // Hiding the Omer is its content row's "Off" since 2026-10-19 (the
    // wording choice moved into that row, without an Off of its own): an old
    // omerMode Off becomes exactly that, with the default wording.
    if (cfg.omerMode == OmerMode::Off) {
        cfg.contentFrequency[(int)ContentType::Omer] = 0;
        cfg.omerMode = OMER_MODE_DEFAULT;
    }

    cfg.webPasswordEnabled = doc["pwdEnabled"] | false;
    strncpy(cfg.webPassword, doc["pwd"] | "", 64);
    cfg.setupComplete = doc["setupComplete"] | false;

    strlcpy(cfg.logLevel, doc["logLevel"] | "info", sizeof(cfg.logLevel));

    CLOCK_LOG("Storage", "Config loaded OK");
    return true;
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
    String tmp = String(path) + ".tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) {
        DiagnosticLog::record("Storage", "Save failed: cannot open %s for writing - %s left unchanged", tmp.c_str(), path);
        return false;
    }
    size_t written = serializeJson(doc, f);
    f.close();

    bool valid = false;
    File check = LittleFS.open(tmp, "r");
    if (check) {
        valid = written > 0 && check.size() == written;
        check.close();
    }
    if (!valid) {
        DiagnosticLog::record("Storage", "Save of %s failed verification (%u bytes written) - previous file kept", path, (unsigned)written);
        LittleFS.remove(tmp);
        return false;
    }
    if (!LittleFS.rename(tmp, path)) {
        DiagnosticLog::record("Storage", "Save failed: rename %s -> %s - previous file kept", tmp.c_str(), path);
        LittleFS.remove(tmp);
        return false;
    }
    return true;
}

// =============================================================================
// saveConfig()
// =============================================================================

// millis() of the oldest change waiting for saveConfigSoon()'s save, 0 = none.
static std::atomic<uint32_t> s_saveSoonSinceMs{0};

void Storage::saveConfigSoon() {
    uint32_t none = 0;
    s_saveSoonSinceMs.compare_exchange_strong(none, millis() | 1);
}

void Storage::loop(const AppConfig& cfg) {
    uint32_t since = s_saveSoonSinceMs;
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
        o["city"] = p.city; o["country"] = p.country;
        o["lat"] = p.lat; o["lon"] = p.lon;
        o["tzid"] = p.tzid; o["tzPOSIX"] = p.tzPOSIX; o["offsetSec"] = p.offsetSec;
    }
    doc["use24Hour"]= cfg.use24Hour;

    JsonArray ntpArr = doc["ntpServers"].to<JsonArray>();
    for (int i = 0; i < cfg.ntpServerCount; i++)
        ntpArr.add(cfg.ntpServers[i]);

    doc["language"] = cfg.language;

    doc["debugTimeOffset"] = cfg.debugTimeOffset;
    doc["devMode"]         = cfg.devMode;
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
    doc["showAll"]            = cfg.showAllMessages;

    JsonArray orderArr = doc["contentOrder"].to<JsonArray>();
    for (int i = 0; i < (int)ContentType::COUNT; i++) orderArr.add(CONTENT_TYPE_KEYS[cfg.contentOrder[i]]);
    JsonObject freqObj = doc["contentFrequency"].to<JsonObject>();
    for (int t = 0; t < (int)ContentType::COUNT; t++) freqObj[CONTENT_TYPE_KEYS[t]] = cfg.contentFrequency[t];

    doc["pwdEnabled"]    = cfg.webPasswordEnabled;
    doc["pwd"]           = cfg.webPassword;
    doc["setupComplete"] = cfg.setupComplete;

    doc["logLevel"] = cfg.logLevel;

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

// Shared by loadEvents() and importEventsBackup(). There's exactly one real
// events.json for this device, already on the current schema (id, jM as an
// int, jYNum as an int) - no older schema can ever reappear, so this reads
// it directly with no backward-compatibility branching.
static void parseEventsFromDoc(JsonDocument& doc, EventList& events) {
    JsonArray arr = doc.as<JsonArray>();
    int count = min((int)arr.size(), MAX_EVENTS);
    events.clear();
    // PsramAllocator (PsramVector.h) throws std::bad_alloc if PSRAM is too
    // full/fragmented to grow the vector - unlike the Web UI's upsert/import
    // handlers, there is no percentage-free pre-check at either of this
    // function's two call sites (boot-time loadEvents(), and /api/restore's
    // importEventsBackup()), so this is the only guard they get. On failure,
    // keep whatever was already parsed rather than losing the whole list.
    try {
        events.reserve(count);
        for (int i = 0; i < count; i++) {
            JsonObject ev = arr[i];
            PersonalEvent e{};

            strncpy(e.id, ev["id"] | "", sizeof(e.id) - 1);
            strncpy(e.name, ev["name"] | "", sizeof(e.name) - 1);
            e.hasGregorian = ev["hasG"] | false;
            e.gDay         = ev["gD"]   | 0;
            e.gMonth       = ev["gM"]   | 0;
            e.gYear        = ev["gY"]   | 0;
            e.afterSunset  = ev["afterSunset"] | false;
            e.hasJewish    = ev["hasJ"] | false;
            e.jDay         = ev["jD"]   | 0;
            e.jMonth       = ev["jM"]   | 0;
            e.jYear        = ev["jYNum"] | 0;
            strncpy(e.icon, ev["icon"] | "", sizeof(e.icon) - 1);
            strncpy(e.image, ev["image"] | "", sizeof(e.image) - 1);
            strncpy(e.type, ev["type"] | "", sizeof(e.type) - 1);
            e.warningDays  = ev["warn"] | 0;

            events.push_back(e);
        }
    } catch (const std::bad_alloc&) {
        DiagnosticLog::record("Storage", "Out of memory loading events - kept %d of %d", (int)events.size(), count);
    }
}

bool Storage::loadEvents(EventList& events) {
    events.clear();
    if (!exists(FS_EVENTS_PATH)) return true;

    File f = LittleFS.open(FS_EVENTS_PATH, "r");
    if (!f) return false;

    FileTools::skipUtf8ByteOrderMark(f);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        DiagnosticLog::record("Storage", "Events file unreadable (%s) - no events until it's fixed", err.c_str());
        return false;
    }

    parseEventsFromDoc(doc, events);
    CLOCK_LOG("Storage", "Loaded %d personal events", (int)events.size());
    return true;
}

bool Storage::saveEvents(const EventList& events) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (const auto& e : events) {
        JsonObject ev = arr.add<JsonObject>();
        ev["id"]   = e.id;
        ev["name"] = e.name;
        ev["hasG"] = e.hasGregorian;
        ev["gD"]   = e.gDay;
        ev["gM"]   = e.gMonth;
        ev["gY"]   = e.gYear;
        ev["afterSunset"] = e.afterSunset;
        ev["hasJ"] = e.hasJewish;
        ev["jD"]   = e.jDay;
        ev["jM"]   = e.jMonth;

        int yVal = e.jYear;
        ev["jYNum"] = yVal;

        char yHeb[32];
        if (yVal >= 5000) JewishDateEngine::yearToGematria(yVal, yHeb, sizeof(yHeb));
        else              JewishDateEngine::numToGematria(yVal, yHeb, sizeof(yHeb));
        ev["jY"] = yHeb;

        ev["icon"] = e.icon;
        ev["image"] = e.image;   // ImageLibrary id, "" = none
        ev["type"] = e.type[0] ? e.type : "birthday";
        ev["warn"] = e.warningDays;
    }

    if (!writeJsonAtomic(FS_EVENTS_PATH, doc)) return false;
    CLOCK_LOG("Storage", "Saved %d personal events", (int)events.size());
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

bool Storage::importBackup(const String& json) {
    // We parse just enough to extract the two sub-sections
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        LOG_AT(LogLevel::WARN, "Storage", "Restore-backup parse error - upload rejected");
        return false;
    }

    bool ok = true;

    if (doc["config"]) {
        String cfgStr;
        serializeJson(doc["config"], cfgStr);
        ok &= importConfigBackup(cfgStr);
    }

    if (doc["events"]) {
        String evStr;
        serializeJson(doc["events"], evStr);
        ok &= importEventsBackup(evStr);
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

bool Storage::importEventsBackup(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    // Parse-then-resave (via the same tolerant parser loadEvents() uses)
    // instead of writing this JSON to disk verbatim, so a backup carrying an
    // older/foreign schema gets normalized to the current on-disk format
    // rather than silently corrupting the event on next load. See
    // parseEventsFromDoc()'s comment for the concrete failure this replaces.
    EventList events;
    parseEventsFromDoc(doc, events);
    return saveEvents(events);
}

// =============================================================================
// Utility
// =============================================================================

bool Storage::exists(const char* path) { return LittleFS.exists(path); }
bool Storage::remove(const char* path) { return LittleFS.remove(path); }
size_t Storage::freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }
