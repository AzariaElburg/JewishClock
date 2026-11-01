#pragma once

#include <Arduino.h>
#include "PsramVector.h"

// =============================================================================
// Config.h
// JewishClock - hardware facts + defaults of persisted settings.
//
// WHAT GOES WHERE:
//   - Here: hardware facts (pins, panel size, bus speeds, I2C address,
//     backlight PWM) and the default value of every setting saved in
//     config.json. If you move a wire, change the number here. Nowhere else.
//   - Every other adjustable value (retry intervals, timeouts, thresholds)
//     lives at the top of the one .cpp that uses it, under a
//     `// === TUNABLES ===` banner, with its unit in its name (_MS, _SEC,
//     _DBM...). It moves to a header only once a second file needs it.
//
//   TO LIST EVERY TUNABLE IN THE PROJECT: search the code for "TUNABLES".
//
//   A value is described only where it's defined; anywhere else, point to it.
//   Pin numbers = GPIO numbers printed on the ESP32-S3 board.
// =============================================================================


// =============================================================================
// FIRMWARE VERSION
// =============================================================================

// FIRMWARE_VERSION: include/Version.h.

// =============================================================================
// CUSTOM GLYPHS
// =============================================================================

// =============================================================================
// ENUMERATIONS (Modes)
// =============================================================================

enum class RolloverMode : int {
    Sunset = 0,
    Tzeit = 1,
    Midnight = 2
};

enum class RotationMode : int {
    Alternating = 0,
    SingleScroll = 1
};

enum class OmerMode : int {
    Off = 0,          // only in old config.json files - loadConfig() turns it into the Omer row's "Off"
    ShortText = 1,
    FullText = 2,
    Abbreviated = 3
};

// Wording of the rain request in ברכת השנים: Ashkenaz "ותן טל ומטר" /
// "ותן ברכה", Sefard "ברך עלינו" / "ברכנו" (Calendar::getRainRequest()).
enum class Nusach : int {
    Ashkenaz = 0,
    Sefard = 1
};

// When an event on a Hebrew date starts and ends: with the Hebrew day (at the
// rollover - צאת הכוכבים by default) or at midnight. Gregorian dates always
// change at midnight. Decided 2026-11-01: the user chooses, default Hebrew day.
enum class EventDayChange : int {
    WithHebrewDay = 0,
    Midnight = 1
};

// Default Mode Constants
// The Hebrew day changes at צאת הכוכבים (after Shabbat/Yom Tov: at Havdalah) -
// the user's father, 2026-10-20 (see Calendar.cpp, applyZmanimPayload).
static constexpr RolloverMode ROLLOVER_MODE_DEFAULT = RolloverMode::Tzeit;
static constexpr RotationMode ROTATION_MODE_DEFAULT = RotationMode::Alternating;
static constexpr OmerMode     OMER_MODE_DEFAULT     = OmerMode::ShortText;
static constexpr Nusach       NUSACH_DEFAULT        = Nusach::Ashkenaz;
static constexpr EventDayChange EVENT_DAY_CHANGE_DEFAULT = EventDayChange::WithHebrewDay;

// ─── Message content types (order + frequency) ─────────────────────────────
//
// Each type on the clock's rotating message list has a display ORDER (its
// position in AppConfig::contentOrder - drag-and-drop reorderable in the web
// UI) and a FREQUENCY (AppConfig::contentFrequency[type], indexed by the enum
// value below): 0 = never show, N = show on every Nth "round". A round is one
// full messageDwellSec-length cycle (see MessageAggregator.cpp) - the same
// round length is used for both Alternating and SingleScroll rotation modes,
// so frequency behaves consistently regardless of which mode is active.
enum class ContentType : uint8_t {
    Events = 0,
    Candle,
    Havdalah,
    Holiday,
    Mevarchim,
    Parasha,
    Omer,
    DafYomi,
    MishnaYomit,
    HebrewDate,
    GregorianDate,
    Fast,                // Fast start/end times, day before through the night after
    PrayerSwitch,        // משיב הרוח (22 Tishrei) / מוריד הטל (15 Nisan), around the switch day
    COUNT
};

// String keys used in config.json / the web API - index-matched to ContentType.
// Defined in Storage.cpp (extern, not `inline`, since this toolchain doesn't
// reliably build with C++17 inline-variable support).
extern const char* const CONTENT_TYPE_KEYS[(int)ContentType::COUNT];


// =============================================================================
// DISPLAY PINS  (SPI → NV3041A via 40-pin FPC breakout)
// =============================================================================
//   FPC Pin (Display) → ESP32-S3 GPIO
//   ---------------------------------------------------------------------------
//   Pin 6  (IOVCC)    → 3.3V     (I/O Power)
//   Pin 7  (VCI)      → 3.3V     (Analog Power)
//   Pin 9  (SPI_CS)   → GPIO 8   (Chip Select)
//   Pin 10 (SPI_SCL)   → GPIO 9   (SPI Clock)
//   Pin 13 (SPI_SDA)   → GPIO 10  (SPI MOSI)
//   Pin 11 (SPI_RS)    → GPIO 11  (Register Select / DC)
//   Pin 15 (RESET)     → GPIO 14  (Hardware Reset)
//   Pin 38 (IM0)       → 3.3V     (SPI Mode Select)
//   Pin 39 (IM1)       → GND      (SPI Mode Select)
//   Pin 40 (SPI4W)     → 3.3V     (Must be tied HIGH for 4-wire mode)
//   Pins 5, 16, 37     → GND
//
// Backlight - NOT driven from a GPIO directly (12 LEDs, up to 180 mA):
//   5V → 12 Ω 1 W resistor → Pin 33 (LED-A); Pins 34-36 (LED-K) → MOSFET
//   module (LR7843) "Load" terminal; GPIO 13 (TFT_BL) → MOSFET PWM input.
//   The MOSFET switches the LED return to GND (low-side), so PWM duty =
//   brightness.
// Full wiring table (connector by connector, incl. the FPC breakout's
// header pin numbers): docs/hardware/ESP32_S3_Clock_Master_Wiring_Matrix_Pinout.docx

static constexpr uint8_t TFT_CS   = 8;
static constexpr uint8_t TFT_SCLK = 9;
static constexpr uint8_t TFT_MOSI = 10;
static constexpr uint8_t TFT_DC   = 11;
static constexpr uint8_t TFT_RST  = 14;
static constexpr uint8_t TFT_BL   = 13;
// FPC pin 8 (TE, tearing effect) -> GPIO 12 (header pin 33), wired
// 2026-10-25, unused by the firmware: see CLAUDE.md, "Scroll tearing".
static constexpr uint8_t TFT_TE   = 12;

// Physical panel resolution
static constexpr uint16_t TFT_WIDTH  = 480;
static constexpr uint16_t TFT_HEIGHT = 128;

// SPI clock to the panel. Only 80 MHz / n is possible (40, 26.7, 20, 16...);
// anything else is rounded down. 40 MHz since 2026-10-17, once the wiring
// was permanent: a full frame now takes ~30 ms to send instead of ~55 ms
// at 20 MHz. (Back on the earlier temporary wiring, 40 MHz left the screen
// blank - if that ever happens again, suspect the wiring and drop to 20.)
// (A live speed switch in Settings -> Advanced never took effect and was
// removed 2026-10-26 - changing the speed means changing it here and reflashing.)
// 80 MHz since 2026-10-26 (the chip datasheet allows a 12.5 ns write cycle,
// docs/hardware/NV3041A_chip_datasheet_V1.2 8.3.2): picture clean (user),
// whole frame 23.4 ms instead of 35.8 ms. The live speed switch in Advanced
// never took effect (frames equally long at 20/40/80 MHz) - only this value counts.
static constexpr uint32_t TFT_SPI_HZ = 80000000UL;

// Backlight PWM (LEDC), all in hardware. 12 bits (2026-10-29, was 8): the
// dark end needs fine steps - 1/255 was already a visible jump in a dark room.
// The ESP32-S3 allows at most 13 bits at 5 kHz (80 MHz / 5 kHz / 2^bits >= 1);
// finer would be pointless anyway - the shortest pulses are shorter than the
// MOSFET module switches. Settings are in % (BrightnessManager), not duty.
static constexpr uint32_t BACKLIGHT_PWM_HZ   = 5000;
static constexpr uint8_t  BACKLIGHT_PWM_BITS = 12;
static constexpr uint32_t BACKLIGHT_DUTY_MAX = (1u << BACKLIGHT_PWM_BITS) - 1;


// =============================================================================
// BH1750 LIGHT SENSOR PINS  (I2C)
// =============================================================================
//   VCC        → 3.3V
//   GND        → GND
//   BH1750 SDA → GPIO 1  (I2C data)   -- moved from GPIO 4 to rule out that pin/header section
//   BH1750 SCL → GPIO 2  (I2C clock)  -- moved from GPIO 5, same reason
//   ADD pin    → GND  (I2C address 0x23)
//
// GPIO 1/2 sit on the DevKitC-1's right-hand header (J3), physically opposite
// the display wiring (which lives on the left header, GPIO 8-14) - easier to
// wire, and a clean test of whether GPIO 4/5 themselves were ever the fault.

static constexpr uint8_t BH1750_SDA  = 1;
static constexpr uint8_t BH1750_SCL  = 2;
static constexpr uint8_t BH1750_ADDR = 0x23;


// =============================================================================
// VISIBLE WINDOW CALIBRATION DEFAULTS
// =============================================================================
//
// The full display is 480×128, but only a 89×15 mm window shows through the
// physical enclosure. These are the measured defaults — fine-tune via Web UI.
//
// WHY ARE THESE IN CONFIG.H AND STORED IN APPCONFIG / config.json?
//   Because they are user-configurable settings that must survive reboots.
//   Storage is the persistence layer for ALL settings, not just "network" ones.
//   DisplayManager reads calibration from AppConfig (loaded by Storage).
//   There is NO separate calib.json — calibration is part of config.json.

static constexpr int DEFAULT_CALIB_X = 16;
static constexpr int DEFAULT_CALIB_Y = 11;
static constexpr int DEFAULT_CALIB_W = 449;
static constexpr int DEFAULT_CALIB_H = 106;
// "Tear lines" in calibration: each frame starts this long after the panel's
// TE pulse (Display.cpp waitForTe()), which places the two still tear lines.
// 12 ms put them just above and below the letters (by eye, 2026-10-26: 80 MHz,
// window y30 h85); a different window moves them - retune with the slider.
static constexpr int DEFAULT_CALIB_TEAR_US = 12000;

// Font is pre-baked at 32×64 px per glyph — no runtime scaling.  See Font.h.


// =============================================================================
// WI-FI & NETWORK
// =============================================================================

#define PROJECT_NAME_DEFAULT         "JewishClock"
#define AP_SSID_PREFIX               "JewishClock-"
#define AP_PASSWORD_DEFAULT          "jewishclock"


// =============================================================================
// NTP TIME SERVERS
// =============================================================================
// Default server list for config.json. Sync policy (step/slew/ignore
// thresholds, intervals): see TUNABLES in NTPManager.cpp.

#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "il.pool.ntp.org"
#define NTP_SERVER_3         "time.cloudflare.com"


// =============================================================================
// BRIGHTNESS CONTROL
// =============================================================================

// All in perceived % (0 = backlight off) - see BrightnessManager.h.
#define BRIGHT_PCT_DEFAULT        80      // manual brightness
#define NIGHT_PCT_DEFAULT         5.0f    // light sensor: faintest (dark room)
#define DAY_PCT_DEFAULT           90.0f   // light sensor: brightest (light room)
// Room light in % (log scale, BrightnessManager::lightPct()): the room's dark
// and light edges until set or learned (20% = 1 lux, 80% = 1000 lux).
#define ROOM_DARK_PCT_DEFAULT     20.0f
#define ROOM_LIGHT_PCT_DEFAULT    80.0f
#define LEARN_ROOM_LIGHT_DEFAULT  true


// =============================================================================
// AMBIENT LIGHT SENSOR (BH1750)
// =============================================================================
// Pins and address: see BH1750 LIGHT SENSOR PINS above.

#define USE_LIGHT_SENSOR_DEFAULT   false


// =============================================================================
// DISPLAY / MESSAGE BEHAVIOR DEFAULTS
// =============================================================================

#define SCROLL_SPEED_DEFAULT       3
#define MESSAGE_DWELL_SEC_DEFAULT  5
#define MESSAGE_SEPARATOR_DEFAULT  " * "

// Proportional-font spacing (see Font.cpp) - gap in pixels after a glyph's
// own ink, added on top of its natural width.
#define FONT_CHAR_GAP_DEFAULT      3   // Between letters within a word.
#define FONT_WORD_GAP_DEFAULT      14  // The space character's own advance (word-to-word gap).
#define FONT_WIDTH_SCALE_DEFAULT   100 // Percent of each glyph's natural ink width to draw at (100 = unchanged).


// =============================================================================
// LANGUAGE
// =============================================================================

#define LANG_EN       "en"
#define LANG_HE       "he"
#define LANG_NL       "nl"
#define LANG_DEFAULT  LANG_EN

#define DEFAULT_TZ_POSIX "IST-2IDT,M3.4.4/26,M10.5.0" // Israel Standard/Daylight Time

// =============================================================================
// JEWISH CALENDAR DEFAULTS
// =============================================================================

#define DEFAULT_LATITUDE   31.7683f
#define DEFAULT_LONGITUDE  35.2137f
#define DEFAULT_IN_ISRAEL  true

// Rain/dew prayer slide: shown this many days before and after the switch
// day (0, 1 or 2). 0 = only on the switch day itself, as the plain phrase.
#define SWITCHOVER_DISPLAY_DAYS     2

#define SHOW_DAF_YOMI_DEFAULT      false
#define SHOW_MISHNA_YOMIT_DEFAULT  false
#define SHOW_MEVARCHIM_DEFAULT     true


// =============================================================================
// STORAGE PATHS
// =============================================================================
//
// All persistent settings live in /config.json.
// Personal events live in /events.json.
//
// There is NO separate /calib.json — calibration is part of /config.json.
// (FS_CALIB_PATH was removed; it was dead code that was never read or written.)

#define FS_CONFIG_PATH  "/config.json"
#define FS_EVENTS_PATH  "/events.json"

// Event pictures: a shared library, FS_IMAGE_DIR/<id>.bmp - see ImageLibrary.h.
#define FS_IMAGE_DIR    "/images"
inline String imagePath(const char* imageId) { return String(FS_IMAGE_DIR "/") + imageId + ".bmp"; }
// Where event photos were kept 2026-10-19..21 (one per event, by event id);
// ImageLibrary::migrateOldPhotos() moves them into the library.
#define FS_OLD_PHOTO_DIR "/photos"
// Waiting area for an update's web files (Updater.cpp).
#define FS_UPDATE_DIR    "/upd"
// The family's replacements for the clock's own pictures (/IMG/<name>.bmp ->
// FS_CUSTOM_DIR/<name>.bmp; Settings > Pictures, 2026-10-21). The clock's own
// stay as the defaults; MessageAggregator's builtinImage() picks.
#define FS_CUSTOM_DIR    "/custom"
inline String customImagePath(const char* name) { return String(FS_CUSTOM_DIR "/") + name + ".bmp"; }
// The diagnostic log (DiagnosticLog.cpp), in a folder of its own since
// 2026-10-25: in the root folder, every log line grew the root's change
// list, and every ~9 lines LittleFS compacted it - ~250 ms of flash work,
// a visible screen hitch.
#define FS_LOG_DIR       "/log"

// The clock's own data - every file it creates itself, and the folders it
// owns - as opposed to the web files that come from Data/ / an update. An
// update never writes or removes these (it removes every OTHER file its
// version doesn't list - Updater.cpp), and they don't count as web files in
// ImageLibrary's room check. A new file the firmware writes must be added
// here, or the next update deletes it.
inline bool isClockDataPath(const String& p) {
    static const char* const files[] = {
        FS_CONFIG_PATH, FS_EVENTS_PATH,
        "/jewish_calendar_data.json", "/backup_tmp.json", "/upload_image.tmp", "/upload_custom.tmp",
    };
    for (const char* f : files) {
        if (p.equalsIgnoreCase(f) || p.equalsIgnoreCase(String(f) + ".tmp")) return true;   // + writeJsonAtomic()'s temp file
    }
    for (const char* d : { FS_IMAGE_DIR, FS_OLD_PHOTO_DIR, FS_UPDATE_DIR, FS_CUSTOM_DIR, FS_LOG_DIR }) {
        if (p.equalsIgnoreCase(d) || p.startsWith(String(d) + "/")) return true;
    }
    return false;
}


// =============================================================================
// HARDWARE REFERENCE
// =============================================================================
//
//  Display:  VIEWE UE039HV-RB40-L009A | 480×128 | NV3041A | SPI 4-wire
//  MCU:      ESP32-S3-DevKitC-1 N16R8 | 16 MB Flash | 8 MB PSRAM
//  Sensor:   BH1750 | I2C | 3.3 V  (wired in; occasional I2C detection failures - see AmbientLight.h)
//  Library:  Arduino_GFX by moononournation

// Pure sanity backstop against a malformed/adversarial events.json claiming
// an absurd element count (e.g. a corrupted file) - NOT an operational
// limit. The real limit is runtime, checked against actual free PSRAM when
// an event is added (see the /api/events upsert handler), since that's the
// resource that actually matters and it adapts to real conditions instead
// of a guessed-in-advance number (EVENTS_PSRAM_*_PCT, TUNABLES in WebUI.cpp).
#define MAX_EVENTS 5000

struct PersonalEvent {
    // 16 lowercase hex digits (a 64-bit value from the ESP32's hardware
    // RNG) + null terminator. Assigned once, server-side, when an event is
    // created; never reused, even after deletion - see generateEventId().
    char id[17];

    char name[128];

    bool  hasGregorian;
    int   gDay, gMonth, gYear;   // gYear 0 = repeat every year
    bool  afterSunset;           // Crucial for anchoring Gregorian births to Hebrew dates

    bool  hasJewish;
    int   jDay, jMonth, jYear;   // jMonth: 1=Tishrei…13=Adar II; jYear 0 = every year
    char  icon[32];              // Display icon / emoji
    char  image[12];             // picture shown instead of the icon: ImageLibrary id, "" = none
    char  type[32];              // birthday, wedding, yahrzeit, other

    int   warningDays;           // 0 = no early warning; >0 show days before
};

// Grows exactly as large as the number of events actually entered (not a
// fixed worst-case reservation), and lives in PSRAM rather than competing
// with the SRAM that WiFi/TLS/the web server/display need - see
// PsramVector.h. Shared by main.cpp (which owns it), WebUI.cpp (which
// mutates it directly on save/delete), MessageAggregator and Calendar
// (which only read it).
using EventList = PsramVector<PersonalEvent>;

// Wi-Fi networks the clock knows (the owner's, the parents', a spare -
// 2026-10-21); it joins whichever of them it hears strongest.
#define WIFI_NETWORKS 3

// Where a known Wi-Fi network is (2026-10-24). The clock's place follows the
// network it's on: joining a network makes that network's place the active
// one (Location.cpp). Looked up once (ip-api) the first time the clock joins
// a network with no place yet; the user can accept or correct it (search).
// No monthly re-lookup: the place doesn't move, and an IP lookup only
// guesses (often the provider's city) - worse for candle-lighting times.
struct Place {
    bool  set;              // false = not known yet
    bool  confirmed;        // accepted or chosen by the user (else: as looked up)
    char  city[48];
    char  country[48];
    float lat, lon;
    char  tzid[48];         // IANA zone name ("Europe/London")
    char  tzPOSIX[64];      // its rule, from FS_TZ_TABLE; "" = not in the table
    int   offsetSec;        // UTC offset when chosen - only used without a rule
};
// zone name -> POSIX rule, made by utils/make_tz_table.js
#define FS_TZ_TABLE "/tz.csv"

struct AppConfig {
    // -- Wi-Fi --
    char     projectName[64];        // Device name (used for mDNS hostname as well)
    // Network 1 - the original single network, keys "wifiSSID"/"wifiPassword"
    // as always. Networks 2-3: extraWifi[], keys "wifiSSID2"/"wifiPassword2"
    // etc. Use wifiSsidAt()/wifiPasswordAt() (below) to go through all three.
    char     wifiSSID[64];
    char     wifiPassword[64];       // Plaintext in RAM; encrypted on flash/backup
    struct { char ssid[64]; char password[64]; } extraWifi[WIFI_NETWORKS - 1];
    int8_t   wifiLastNetwork;        // the one connected last (0..2), tried first; -1 = none yet
    Place    places[WIFI_NETWORKS];  // where each network is (see Place)

    // -- Location: the active place (the connected network's - Location.cpp) --
    char     locationCountry[64];
    char     locationCity[64];
    float    latitude;
    float    longitude;
    char     tzid[64];               // IANA Timezone (e.g. "Asia/Jerusalem")
    char     tzPOSIX[128];           // POSIX TZ string for DST
    RolloverMode rolloverMode;
    bool     inIsrael;               // Israeli parasha schedule + zmanim rules

    // -- Time --
    bool     use24Hour;
    char     ntpServers[4][64];
    int      ntpServerCount;
    bool     manualTimeSet;

    // -- Language --
    char     language[4];            // "en" | "he" | "nl"

    // -- Display behavior --
    int64_t  debugTimeOffset;        // Shift from real UTC in seconds (64-bit: tests up to year 2300+)
    RotationMode rotationMode;       
    int      messageDwellSec;        // Seconds per message (alternating mode)
    char     messageSeparator[16];   // Between messages in scroll mode
    int      scrollSpeed;            // Pixels per animation frame
    int      fontCharGapPx;          // Proportional font: px gap between letters within a word
    int      fontWordGapPx;          // Proportional font: px gap between words (space advance)
    int      fontWidthScalePct;      // Proportional font: glyph width, as % of its natural ink width

    // -- Brightness --
    bool     devMode;                // Persisted Developer Mode
    // Perceived % (0 = off) - see BrightnessManager.h. (The time-based night
    // mode and the old raw 0-255 values are gone since 2026-10-29.)
    int      brightPct;              // manual
    bool     useLightSensor;         // follow the room's light (BH1750)
    float    nightPct;               // ... faintest level (at the dark edge and below)
    float    dayPct;                 // ... brightest level (at the light edge and above)
    float    roomDarkPct;            // room light % that counts as dark
    float    roomLightPct;           // room light % that counts as light
    bool     learnRoomLight;         // the two edges follow the room

    // -- Calibration --
    int      calibX, calibY, calibW, calibH;
    int      calibTearUs;            // DEFAULT_CALIB_TEAR_US

    // -- Jewish calendar display toggles --
    bool     showHebrewYear;         // "Show year" in the UI: include the year in TODAY's Hebrew AND Gregorian date slides (other dates/events are unaffected). Name kept for config.json compatibility.
    bool     showShabbatShalom;      // Enabled/Disable "Shabbat Shalom" greeting
    bool     showRoshChodesh;
    bool     showFasts;
    OmerMode omerMode;
    bool     showSpecialShabbat;
    bool     showLiturgical;         // יעלה ויבוא, הלל, אין תחנון…
    bool     showSwitchoverPhrases;  // משיב הרוח, טל ומטר
    int      switchoverWindowDays;   // Rain/dew prayer slide: days before/after the switch day, 0-2 (see SWITCHOVER_DISPLAY_DAYS)
    Nusach   nusach;                 // rain request wording (NUSACH_DEFAULT)
    EventDayChange eventDayChange;   // Hebrew-date events: with the Hebrew day or at midnight
    bool     showAllMessages;        // Master on/off

    // -- Message content order + frequency (see ContentType in Config.h) --
    uint8_t  contentOrder[(int)ContentType::COUNT];     // Display order, as ContentType values
    uint8_t  contentFrequency[(int)ContentType::COUNT]; // Indexed by ContentType: 0=off, N=every Nth round

    // -- Security --
    bool     webPasswordEnabled;
    char     webPassword[64];

    // -- First-boot flag --
    bool     setupComplete;

    // -- Debugging --
    // Persisted boot-time default only (see LogLevel.h). A live change from
    // the Settings UI applies immediately but, unless explicitly marked to
    // persist, stays RAM-only and reverts to whatever's here on any reboot -
    // this field is what it reverts TO. "none"/"error"/"warn"/"info"/"debug".
    char     logLevel[8];
};

// Known Wi-Fi network i (0 .. WIFI_NETWORKS-1); "" = slot empty.
inline char* wifiSsidAt(AppConfig& c, int i)     { return i == 0 ? c.wifiSSID : c.extraWifi[i - 1].ssid; }
inline char* wifiPasswordAt(AppConfig& c, int i) { return i == 0 ? c.wifiPassword : c.extraWifi[i - 1].password; }
inline const char* wifiSsidAt(const AppConfig& c, int i)     { return i == 0 ? c.wifiSSID : c.extraWifi[i - 1].ssid; }
inline const char* wifiPasswordAt(const AppConfig& c, int i) { return i == 0 ? c.wifiPassword : c.extraWifi[i - 1].password; }
// Config keys: "wifiSSID" / "wifiSSID2" / "wifiSSID3" (same for wifiPassword).
inline String wifiConfigKey(const char* base, int i) { return i == 0 ? String(base) : String(base) + String(i + 1); }
