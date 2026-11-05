#pragma once

// =============================================================================
// Calendar.h
// JewishClock — Calendar Manager (Stateful Orchestrator).
//
// ARCHITECTURE: Hybrid approach
//   • Hebrew date:   Calculated via stateless JewishDateEngine.
//   • Everything else (parasha, holidays, candle lighting, havdalah, zmanim,
//     Daf Yomi, Mishna Yomit, special Shabbatot, Rosh Chodesh, Omer):
//     fetched from the Hebcal JSON API (hebcal.com) and cached in LittleFS.
//
// WHY HEBCAL FOR MOST THINGS?
//   Jewish calendar postponement rules (דחיות), parasha doubling, candle
//   lighting minutes, Daf Yomi page numbers, and Israel vs Diaspora
//   differences involve hundreds of edge cases. Hebcal has maintained these
//   correctly for 30+ years and is used by millions. There is no good reason
//   to reimplement them on an embedded device when an authoritative API is free.
//
// OFFLINE BEHAVIOUR:
//   A full month's data is cached to /hebcal_cache.json on first successful
//   fetch and re-fetched every 24 hours (or on demand). While offline, the
//   cache serves stale data; Hebrew date is always computed locally.
//
// HEBCAL API ENDPOINT (see actual request in Calendar.cpp for the literal string):
//   GET https://www.hebcal.com/hebcal?v=1&cfg=json
//       &start=YYYY-MM-DD&end=YYYY-MM-DD
//       &geo=pos&latitude=LAT&longitude=LON&tzid=UTC
//       &c=on      (candle lighting)
//       &M=on      (havdalah, at nightfall)
//       &o=on      (Sefirat HaOmer)
//       &ss=on     (special Shabbatot)
//       &d=on      (Hebrew date for entire range)
//       &myomi=on  (Mishna Yomit)
//       &mf=on     (minor fasts)
//       &mod=on    (modern holidays - Yom HaShoah, Yom HaAtzma'ut, etc.)
//       &s=on      (weekly parasha)
//       &F=on      (Daf Yomi)
//       &maj=on    (major holidays - Rosh Hashanah, Yom Kippur, Pesach, Shavuot, Sukkot)
//       &min=on    (minor holidays - Chanukah, Purim, Tu BiShvat, Lag B'Omer, ...)
//       &nx=on     (Rosh Chodesh)
//       &mvch=on   (Shabbat Mevarchim)
//       &i=on/off  (Israel vs. Diaspora schedule, from cfg.inIsrael)
//       &lg=he     (Hebrew event titles)
// =============================================================================

#include <Arduino.h>
#include "Storage.h"
#include "JewishDateEngine.h"
#include <vector>

// ─── Full day info ────────────────────────────────────────────────────────────

struct DayInfo {
    char gregorianDate[11];   // "YYYY-MM-DD"
    char hebrewDateStr[64];   // Authoritative string
    char parashaName[80];     // "Parashat Tzav" (English)
    char parashaHebrew[80];   // "פרשת צו"
    // The day's holidays, each shown as its own slide: up to two (25 years of
    // Hebcal data never have more on one day - e.g. Ta'anit Esther + Erev
    // Purim), a Yom Tov first, otherwise in Hebcal's order. "" = none.
    char holidayHebrew[80];
    char holidayHebrew2[80];
    char mevarchimHebrew[80]; // "מברכים חודש חשוון" or "" - only set on the Shabbat Mevarchim itself
    char sunset[10];          // "HH:MM:SS"
    char tzeit[10];           // "HH:MM:SS"
    char fastBegin[10];       // "HH:MM" - Hebcal "Fast begins" on this civil day, or ""
    char fastEnd[10];         // "HH:MM" - Hebcal "Fast ends" on this civil day, or ""
    char fastName[48];        // Fast's Hebrew name (from the begin/end item's memo), or ""
    char candleLightStr[10];  // "HH:MM:SS"
    char havdalahStr[10];     // "HH:MM:SS"
    char omerStr[80];         // "Day 7 of the Omer" or ""
    char omerHebrew[160];     // Hebrew Omer text or "" (Increased for Nikud support)
    int  omerDay;
    char dafYomi[48];         // "Sanhedrin 102" or ""
    char mishnaYomit[48];     // "Berachot 1:1" or ""
    char liturgy[256];        // Combined notes: "משיב הרוח, הלל"
    bool isYomTov;            // melacha prohibited (biblical/rabbinic Yom Tov)
    int  hYear;               // Numeric year (e.g. 5784)
    int  hMonth;              // 1-13
    int  hDay;                // 1-30
    bool isLeapYear;
    bool isFast;
    bool isRoshChodesh;
    bool isSpecialShabbat;    // Shekalim/Zachor/Parah/HaChodesh/HaGadol
    bool isShabbat;
};

// What the fast-times slide should say on the current civil day. Times come
// straight from Hebcal ("Fast begins"/"Fast ends", Israel rules in Israel
// mode). Shown from midnight the day before a fast until midnight after it.
struct FastNotice {
    enum class Kind { None, Fast, SeudaMafseket };
    Kind kind = Kind::None;
    char begin[10] = "";      // "" = not shown (already started yesterday)
    bool beginTomorrow = false;
    char end[10] = "";        // "" = Hebcal gives no end (Ta'anit Bechorot, Friday fasts)
    bool endTomorrow = false;
    char name[48] = "";       // Used as the label when there's no end time
};

// Rain/dew prayer switch: משיב הרוח from Musaf of Shemini Atzeret (22 Tishrei), מוריד
// הטל from Musaf of the first day of Pesach (15 Nisan) - fixed Hebrew dates.
// Counted in civil days (midnight to midnight, the user 2026-11-02): -2
// "ממחרתיים", -1 "ממחר", 0 "ממוסף" (Musaf only on the day itself), after it
// the phrase alone.
struct PrayerSwitchNotice {
    const char* phrase = nullptr;   // nullptr = nothing to show today
    int daysFromSwitch = 0;         // -2..2: negative = the switch is still ahead
};

// The rain request in ברכת השנים (a separate switch from the one above),
// from the first weekday Maariv: winter wording from Maariv of 7 Cheshvan in
// Israel, of 4 December abroad (5 December before a leap year; a day later
// from 2100); summer wording from Maariv after Yom Tov of Pesach - on Yom
// Tov there is no ברכת השנים. Calendar::getRainRequest().
struct RainRequestNotice {
    const char* phrase = nullptr;   // nullptr = nothing to show today
    int daysFromSwitch = 0;         // civil days from the day whose evening switches:
                                    // -2/-1 "ממחרתיים/ממחר בערב", 0 "מהערב", after it the phrase alone
};

// ─── Calendar API ─────────────────────────────────────────────────────────────

class Calendar {
public:
    static void begin(const AppConfig& cfg);
    // cfg is non-const: a completed location lookup is written into it
    // (Location::applyLookup()).
    static void update(AppConfig& cfg, const EventList& events);

    // State Accessors (Pure Getters)
    static const DayInfo&  getToday();
    // Candle lighting and Havdalah specifically: keyed to the plain civil
    // (Gregorian, local-midnight-to-midnight) date, NOT the sunset/tzeit
    // rollover getToday() uses for the Hebrew date/holiday/parasha/etc. Those
    // two fields are tagged by Hebcal to the Gregorian date they occur on,
    // and each only becomes chronologically relevant near the END of that
    // civil day - using the sunset-rollover-adjusted day for them made
    // Havdalah appear the instant the new Hebrew day began (up to ~24h
    // before it actually happens) while erasing that day's own candle time
    // the same instant, hours before midnight.
    static const DayInfo&  getCivilToday();
    // Parasha and Mevarchim specifically: Hebcal tags each only onto the
    // Saturday it's actually read/announced on, so getToday()/getCivilToday()
    // only ever see it on that one day. These instead find the nearest
    // Shabbat from today onward (today itself counts) and return THAT one
    // day's field, so the upcoming Shabbat's parasha (and, when there is
    // one, the upcoming Shabbat Mevarchim) can be shown all week, not just
    // on the day itself - and, on the Shabbat itself, this still returns
    // that same day's value, never jumping ahead to next week's.
    static String getUpcomingParasha();
    static String getUpcomingMevarchim();
    static const char*     getTodaySummary();
    static FastNotice getFastNotice();
    // windowDays (0-2): how many days before/after the switch day to show it.
    // civilTodayAbs: today's civil date (absolute day) - both count civil days.
    // Ashkenazim abroad say nothing in summer: no מוריד הטל for them.
    static PrayerSwitchNotice getPrayerSwitch(int windowDays, Nusach nusach, bool inIsrael, long civilTodayAbs);
    static RainRequestNotice getRainRequest(int windowDays, Nusach nusach, bool inIsrael, long civilTodayAbs);

    // Side Effects
    static void clearCacheAndRefresh();
    static void forceRefresh();
    static void timeJumped();   // the debug time changed: fetch at once if the new date isn't in the saved calendar
    // One location lookup (ip-api, background) for network `slot` - the one
    // the clock is on; the answer goes to Location::applyLookup(). Started by
    // update() as soon as no other download runs. Any task.
    static void requestLookup(int slot);
    static String formatUserHebrewDate(const char* fullDate);

    // Timezone helper derived from configured location

private:
    static DayInfo       s_today;
    static DayInfo       s_civilToday;
    static std::vector<DayInfo> s_manifest;
    static unsigned long s_lastFetchMs;
    static bool          s_forceRefresh;

    // ── Date computation ──
    static void syncToday(const AppConfig& cfg);
    static void computeLocalHebrew(int y, int m, int d, bool pastRollover);

    // ── Hebcal fetch & cache ──
    static bool   loadCache();
    static void   saveCache();
};
