// =============================================================================
// lang.cpp
// JewishClock - Language string table definitions.
//
// Hebrew strings are stored as UTF-8 byte sequences.
// The display renderer handles RTL automatically when Hebrew codepoints
// are detected.
// =============================================================================

#include "lang.h"
#include <string.h>

// ─── English ──────────────────────────────────────────────────────────────────

static const char* const s_prayerSwitchWhenEn[5] = {
    "from the day after tomorrow", "from tomorrow", "from today", "since yesterday", "since 2 days ago"
};
static const char* const s_prayerSwitchWhenHe[5] = {
    "מ-מחרתיים", "מ-מחר", "מהיום", "מ-אתמול", "מ-שלשום"
};
static const char* const s_prayerSwitchWhenNl[5] = {
    "vanaf overmorgen", "vanaf morgen", "vanaf vandaag", "sinds gisteren", "sinds eergisteren"
};

static const char* const s_gregorianMonthsEn[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char* const s_gregorianMonthsHe[12] = {
    "ינואר", "פברואר", "מרץ", "אפריל", "מאי", "יוני",
    "יולי", "אוגוסט", "ספטמבר", "אוקטובר", "נובמבר", "דצמבר"
};

static const char* const s_gregorianMonthsNl[12] = {
    "januari", "februari", "maart", "april", "mei", "juni",
    "juli", "augustus", "september", "oktober", "november", "december"
};

const DisplayStrings STRINGS_EN = {
    /* parasha         */ "Parashat",
    /* candleLighting  */ "Candle lighting",
    /* havdalah        */ "Havdalah",
    /* roshChodesh     */ "Rosh Chodesh",
    /* chagSameach     */ "Chag Sameach",
    /* shabbatShalom   */ "Shabbat Shalom",
    /* goodYomTov      */ "Good Yom Tov",
    /* tzomKal         */ "Tzom Kal",
    /* omerDay         */ "Day %d of the Omer",
    /* dafYomi         */ "Daf Yomi",
    /* mishnaYomit     */ "Mishna Yomit",
    /* birthday        */ "Birthday",
    /* yahrzeit        */ "Yahrzeit",
    /* anniversary     */ "Anniversary",
    /* omerOf          */ "of the Omer",
    /* omerIn          */ "in",
    /* daySingular     */ "day",
    /* dayPlural       */ "days",
    /* whichAre        */ "which are",
    /* weekSingular    */ "week",
    /* weekPlural      */ "weeks",
    /* plus            */ "+",
    /* fastLabel       */ "Fast",
    /* tomorrow        */ "tomorrow",
    /* until           */ "until",
    /* seudaMafseket   */ "Pre-fast meal until",
    /* gregorianMonths */ s_gregorianMonthsEn,
    /* prayerSwitchWhen */ s_prayerSwitchWhenEn,
};

// ─── Hebrew (UTF-8) ───────────────────────────────────────────────────────────
// Each string is valid UTF-8.  RTL rendering is handled by DisplayManager.

const DisplayStrings STRINGS_HE = {
    /* parasha         */ "פרשת",
    /* candleLighting  */ "הדלקת נרות",
    /* havdalah        */ "הבדלה",
    /* roshChodesh     */ "ראש חודש",
    /* chagSameach     */ "חג שמח",
    /* shabbatShalom   */ "שבת שלום",
    /* goodYomTov      */ "יום טוב",
    /* tzomKal         */ "צום קל",
    /* omerDay         */ "יום %d לעומר",
    /* dafYomi         */ "דף יומי",
    /* mishnaYomit     */ "משנה יומית",
    /* birthday        */ "יום הולדת",
    /* yahrzeit        */ "יארצייט",
    /* anniversary     */ "יום נישואין",
    /* omerOf          */ "לעומר",
    /* omerIn          */ "בעוד",
    /* daySingular     */ "יום",
    /* dayPlural       */ "ימים",
    /* whichAre        */ "שהם",
    /* weekSingular    */ "שבוע",
    /* weekPlural      */ "שבועות",
    /* plus            */ "+", // +
    /* fastLabel       */ "צום",
    /* tomorrow        */ "מחר",
    /* until           */ "עד",
    /* seudaMafseket   */ "סעודה מפסקת עד",
    /* gregorianMonths */ s_gregorianMonthsHe,
    /* prayerSwitchWhen */ s_prayerSwitchWhenHe,
};

// ─── Dutch ────────────────────────────────────────────────────────────────────

const DisplayStrings STRINGS_NL = {
    /* parasha         */ "Parasja",
    /* candleLighting  */ "Kaarsjes aansteken",
    /* havdalah        */ "Havdala",
    /* roshChodesh     */ "Rosj Chodeesj",
    /* chagSameach     */ "Chag Sameach",
    /* shabbatShalom   */ "Sjabbat Sjalom",
    /* goodYomTov      */ "Goed Joem Tov",
    /* tzomKal         */ "Tzom Kal",
    /* omerDay         */ "Dag %d van de Omer",
    /* dafYomi         */ "Daf Jomi",
    /* mishnaYomit     */ "Misjna Jomit",
    /* birthday        */ "Verjaardag",
    /* yahrzeit        */ "Jahrzeit",
    /* anniversary     */ "Trouwdag",
    /* omerOf          */ "van de Omer",
    /* omerIn          */ "over",
    /* daySingular     */ "dag",
    /* dayPlural       */ "dagen",
    /* whichAre        */ "die zijn",
    /* weekSingular    */ "week",
    /* weekPlural      */ "weken",
    /* plus            */ "+",
    /* fastLabel       */ "Vasten",
    /* tomorrow        */ "morgen",
    /* until           */ "tot",
    /* seudaMafseket   */ "Laatste maaltijd tot",
    /* gregorianMonths */ s_gregorianMonthsNl,
    /* prayerSwitchWhen */ s_prayerSwitchWhenNl,
};

// ─── Lookup ───────────────────────────────────────────────────────────────────

const DisplayStrings& getStrings(const char* langCode) {
    if (langCode && strcmp(langCode, LANG_HE) == 0) return STRINGS_HE;
    if (langCode && strcmp(langCode, LANG_NL) == 0) return STRINGS_NL;
    return STRINGS_EN;  // Default
}
