#pragma once

// =============================================================================
// lang.h
// JewishClock - Multi-language display string support.
//
// This covers strings that appear ON THE CLOCK DISPLAY.
// Web UI localisation is handled separately in index.html (JS dictionaries).
//
// Supported: English (en), Hebrew (he), Dutch (nl).
//
// Usage:
//   const DisplayStrings& s = getStrings(cfg.language);
//   // sprintf buffer, then: DisplayManager::setMessage(buf);
//   char buf[64];
//   snprintf(buf, sizeof(buf), s.candleLighting);
//   // or, for omer:
//   snprintf(buf, sizeof(buf), s.omerDay, dayNumber);
// =============================================================================

#include <Arduino.h>
#include "Config.h"

// ─── Display string table ─────────────────────────────────────────────────────

struct DisplayStrings {

    // Jewish calendar messages
    const char* parasha;            // "Parashat" / "פרשת" / "Parasja"
    const char* candleLighting;     // "Candle lighting" / "הדלקת נרות" / "Kaarsjes aansteken"
    const char* havdalah;           // "Havdalah" / "הבדלה" / "Havdala"
    const char* roshChodesh;        // "Rosh Chodesh" / "ראש חודש" / "Rosj Chodeesj"
    const char* chagSameach;        // "Chag Sameach" / "חג שמח" / "Chag Sameach"
    const char* shabbatShalom;      // "Shabbat Shalom" / "שבת שלום" / "Sjabbat Sjalom"
    const char* goodYomTov;         // "Good Yom Tov" / "יום טוב" / "Goed Joem Tov"
    const char* tzomKal;            // "Tzom Kal" / "צום קל" / "Tzom Kal"

    // Omer — printf format, %d = day number
    const char* omerDay;            // "Day %d of the Omer"

    // Learning
    const char* dafYomi;            // "Daf Yomi" / "דף יומי" / "Daf Jomi"
    const char* mishnaYomit;        // "Mishna Yomit" / "משנה יומית" / "Misjna Jomit"

    // Personal events
    const char* birthday;           // "Birthday" / "יום הולדת" / "Verjaardag"
    const char* yahrzeit;           // "Yahrzeit" / "יארצייט" / "Jahrzeit"
    const char* anniversary;        // "Anniversary" / "יום נישואין" / "Trouwdag"

    // Omer and Event-related strings
    const char* omerOf;          // "of the Omer" / "לעומר"
    const char* omerIn;          // "in" / "בעוד"
    const char* daySingular;     // "day" / "יום"
    const char* dayPlural;       // "days" / "ימים"
    const char* whichAre;        // "which are" / "שהם"
    const char* weekSingular;    // "week" / "שבוע"
    const char* weekPlural;      // "weeks" / "שבועות"
    const char* plus;            // "+"

    // Fast-times slide - kept short for the display
    const char* fastLabel;          // "צום" / "Fast" / "Vasten"
    const char* tomorrow;           // "מחר" / "tomorrow" / "morgen"
    const char* until;              // "עד" / "until" / "tot"
    const char* seudaMafseketUntil; // "סעודה מפסקת עד" / "Pre-fast meal until" / "Laatste maaltijd tot"

    // Gregorian month names, index 0 = January. Used for the Gregorian date
    // slide, formatted as "<day> <month> <year>".
    const char* const* gregorianMonths;

    // Rain/dew prayer slide (Calendar::getPrayerSwitch() / getRainRequest()).
    // The phrases themselves stay Hebrew in every language. Index = days
    // ahead (0..2): "from Musaf" (on the day), "from tomorrow", ...
    const char* const* prayerSwitchFrom;
    // ותן טל ומטר starts at Maariv: index 0..2 = this evening .. the evening
    // after tomorrow.
    const char* const* prayerSwitchEvening;
};

// ─── String tables ────────────────────────────────────────────────────────────

extern const DisplayStrings STRINGS_EN;
extern const DisplayStrings STRINGS_HE;
extern const DisplayStrings STRINGS_NL;

// Returns the appropriate string table for the given language code.
// Falls back to English for unknown codes.
const DisplayStrings& getStrings(const char* langCode);
