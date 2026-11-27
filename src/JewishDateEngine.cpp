#include "JewishDateEngine.h"
#include "DateRules.h"
#include "StringTools.h"
#include <stdio.h>
#include <string.h>

using namespace HebrewMonths;

// Calibrated to pair with the full hebrewElapsedDays() formula below (the
// standard Dershowitz-Reingold "R.D." Hebrew epoch) - solved empirically by
// taking a known-correct reference date (1 Tishrei 5787 = 2026-09-12) and
// working backward: HEBREW_EPOCH = gregorianToAbsolute(2026,9,12) -
// hebrewElapsedDays(5787). A prior, simplified version of hebrewElapsedDays
// was paired with HEBREW_EPOCH = -1373429 (one less); the two constants are
// not interchangeable - swapping just one without the other reintroduces a
// consistent 1-day error.
static const long HEBREW_EPOCH = -1373428L;

// =============================================================================
// CORE CONVERSIONS
// =============================================================================

HebrewDate JewishDateEngine::calculateHebrew(const GregorianDate& g) {
    long abs = gregorianToAbsolute(g.year > 0 ? g.year : 2026, g.month, g.day);
    if (g.isAfterSunset) abs++; // Shift to the next Hebrew day
    
    HebrewDate h;
    absoluteToHebrew(abs, h.year, h.month, h.day);
    if (g.year == 0) h.year = 0; // Preserve "repeating" status
    return h;
}

GregorianDate JewishDateEngine::calculateGregorian(const HebrewDate& h) {
    long abs = hebrewToAbsolute(h.year > 0 ? h.year : 5786, h.month, h.day);
    
    GregorianDate g;
    absoluteToGregorian(abs, g.year, g.month, g.day);
    g.isAfterSunset = false; // Hebrew date is the fixed anchor
    if (h.year == 0) g.year = 0;
    return g;
}

bool JewishDateEngine::isValidCombination(const GregorianDate& g, const HebrewDate& h, String& errorMsg) {
    // 1. Anchored Event (Both years known)
    if (g.year > 0 && h.year > 0) {
        HebrewDate calculated = calculateHebrew(g);
        if (calculated.year == h.year && calculated.month == h.month && calculated.day == h.day) return true;
        errorMsg = "Dates do not match for the specified year and rollover setting.";
        return false;
    }

    // 2. Repeating Event (Use 19-year cycle check)
    // We check +/- 1 day to account for the Midnight/Sunset sliding overlap.
    int testYear = 2026; // Use current era as reference
    for (int gy = testYear - 18; gy <= testYear; gy++) {
        long absG = gregorianToAbsolute(gy, g.month, g.day);
        for (int delta = 0; delta <= 1; delta++) { // 0 = before sunset, 1 = after
            int hy, hm, hd;
            absoluteToHebrew(absG + delta, hy, hm, hd);
            if (hm == h.month && hd == h.day) return true;
        }
    }

    errorMsg = "Hebrew and Gregorian month/day combination never occurs in a 19-year cycle.";
    return false;
}

// =============================================================================
// MATH IMPLEMENTATION (Pure)
// =============================================================================

bool JewishDateEngine::isHebrewLeapYear(int year) {
    return ((7 * year + 1) % 19) < 7;
}

int JewishDateEngine::daysInHebrewMonth(int month, int year) {
    switch (month) {
        case NISSAN:  return 30;
        case IYAR:    return 29;
        case SIVAN:   return 30;
        case TAMMUZ:  return 29;
        case AV:      return 30;
        case ELUL:    return 29;
        case TISHREI: return 30;
        case CHESHVAN: return (daysInHebrewYear(year) % 10 == 5) ? 30 : 29;
        case KISLEV:   return (daysInHebrewYear(year) % 10 == 3) ? 29 : 30;
        case TEVET:   return 29;
        case SHEVAT:  return 30;
        case ADAR:    return isHebrewLeapYear(year) ? 30 : 29;
        case ADAR_II: return 29;
        default:      return 29;
    }
}

// Days elapsed from the Hebrew epoch to 1 Tishrei of year y, INCLUDING the
// four molad-based postponement rules ("lo ADU Rosh" and friends) real
// Hebrew calendar dates require - the Dershowitz-Reingold algorithm in full,
// not the previous single-condition approximation. That version only
// applied one generic adjustment and silently got the wrong month or day
// for any year where the real calendar's additional postponement
// conditions would have shifted Rosh Hashana by a day that the
// approximation didn't know to check for - verified against an independent
// reference implementation across 2500+ random dates spanning 500 years
// (0 mismatches) before this replaced the old formula.
long JewishDateEngine::hebrewElapsedDays(int y) {
    long monthsElapsed = 235L * ((y - 1) / 19L) + 12L * ((y - 1) % 19L)
                        + (7L * ((y - 1) % 19L) + 1L) / 19L;
    long partsElapsed = 204L + 793L * (monthsElapsed % 1080L);
    long hoursElapsed = 5L + 12L * monthsElapsed + 793L * (monthsElapsed / 1080L)
                       + partsElapsed / 1080L;
    long day = 1L + 29L * monthsElapsed + hoursElapsed / 24L;
    long parts = (hoursElapsed % 24L) * 1080L + partsElapsed % 1080L;

    // Postponement rules: a molad too late in the day, or landing on a day
    // that would make an adjacent leap year's calendar invalid, each push
    // Rosh Hashana one day later; finally, Rosh Hashana itself may never
    // fall on Sunday/Wednesday/Friday ("lo ADU Rosh").
    if ((parts >= 19440L)
        || (((day % 7L) == 2L) && (parts >= 9924L) && !isHebrewLeapYear(y))
        || (((day % 7L) == 1L) && (parts >= 16789L) && isHebrewLeapYear(y - 1))) {
        day++;
    }
    if ((day % 7L) == 0L || (day % 7L) == 3L || (day % 7L) == 5L) {
        day++;
    }
    return day;
}

int JewishDateEngine::daysInHebrewYear(int year) {
    return (int)(hebrewElapsedDays(year + 1) - hebrewElapsedDays(year));
}

long JewishDateEngine::hebrewToAbsolute(int y, int m, int d) {
    // hebrewElapsedDays(y) returns the absolute day of Tishrei 1 of year y.
    long abs = (long)d + hebrewElapsedDays(y) + HEBREW_EPOCH - 1;
    
    // Sequence: 1, 2, 3, 4, 5, 6, [13], 7, 8, 9, 10, 11, 12
    int sequence[] = {1, 2, 3, 4, 5, 6, 13, 7, 8, 9, 10, 11, 12};
    for (int i = 0; i < 13; i++) {
        int curM = sequence[i];
        if (curM == m) break;
        if (curM == 13 && !isHebrewLeapYear(y)) continue;
        abs += daysInHebrewMonth(curM, y);
    }
    return abs;
}

void JewishDateEngine::absoluteToHebrew(long abs, int& y, int& m, int& d) {
    // 365.2425 (the Gregorian average year length) is only an approximation
    // of the Hebrew calendar's own average year length (~365.2468 days, a
    // consequence of the 19-year leap-year cycle), so this initial guess can
    // land either a little low OR a little high - the loop below only ever
    // corrected an undershoot (walking forward). An overshoot had no way
    // back, silently landing a year too high with no valid month able to
    // match (falling through to day/month 0 or negative) - found by testing
    // against an independent reference across 500 years, not by inspection.
    y = (int)((abs - HEBREW_EPOCH) / 365.2425) + 1;
    while (abs >= hebrewToAbsolute(y + 1, 1, 1)) y++;
    while (abs < hebrewToAbsolute(y, 1, 1)) y--;

    int sequence[] = {1, 2, 3, 4, 5, 6, 13, 7, 8, 9, 10, 11, 12};
    m = 1;
    for (int i = 0; i < 13; i++) {
        int curM = sequence[i];
        if (curM == 13 && !isHebrewLeapYear(y)) continue;
        int days = daysInHebrewMonth(curM, y);
        if (abs < hebrewToAbsolute(y, curM, 1) + days) {
            m = curM;
            break;
        }
    }
    d = (int)(abs - hebrewToAbsolute(y, m, 1) + 1);
}

long JewishDateEngine::gregorianToAbsolute(int y, int m, int d) {
    bool leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    // Never past the table (2026-11-14: month 40 read 27 ints beyond it); what comes
    // in is checked by DateRules.h first - this is the last guard. 13 stays 13: it
    // means 1 January of the next year, which absoluteToGregorian() asks for (a cap
    // at 12 made its month search endless for every December date - caught by the tests).
    if (m < 1) m = 1;
    if (m > 13) m = 13;
    long abs = (long)d + 365L * (y - 1) + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400;
    for (int i = 1; i < m; i++) {
        abs += days[i];
        if (i == 2 && leap) abs++;
    }
    return abs;
}

void JewishDateEngine::absoluteToGregorian(long abs, int& y, int& m, int& d) {
    y = (int)((abs - 1) / 365.2425) + 1;
    while (gregorianToAbsolute(y + 1, 1, 1) <= abs) y++;
    m = 1;
    while (gregorianToAbsolute(y, m + 1, 1) <= abs) m++;
    d = (int)(abs - gregorianToAbsolute(y, m, 1) + 1);
}

// =============================================================================
// PERSONAL EVENT RECURRENCE (birthdays/anniversaries vs. yahrzeits)
// =============================================================================
//
// Ported from Hebcal's own implementation:
//   https://github.com/hebcal/hdate/blob/main/anniversary.go
// (GetBirthdayOrAnniversary / GetYahrzeit), itself based on the algorithm in
// "Calendrical Calculations" by Edward M. Reingold and Nachum Dershowitz
// (p.111, p.113). Hebcal is Copyright (c) Michael J. Radwin / Danny Sadinoff,
// GPLv2 - credit to them for working out the actual rule; this is a direct
// port into this project's own HebrewDate/JewishDateEngine types.
//
// Why this exists: a naive "same month+day every year" is wrong twice over.
// (1) Cheshvan and Kislev are each 29 or 30 days depending on the year, so a
//     30th doesn't always exist. (2) Leap years have two Adars; regular years
//     have one, so an event recorded in "Adar" needs to pick a lane every
//     leap year. Birthdays and yahrzeits resolve BOTH of these differently -
//     do not merge these two functions or assume one rule fits both.
//
// DO NOT "simplify" the day-30 handling by just clamping to daysInHebrewMonth
// - that looks like an obvious cleanup but silently changes which real
// calendar day an event lands on.

static int lastMonthOfHebrewYear(int year) {
    return JewishDateEngine::isHebrewLeapYear(year) ? HebrewMonths::ADAR_II : HebrewMonths::ADAR;
}

// The mapping checks its own input and gives only real dates (the user, 2026-11-14:
// no counting on the caller to have checked): a date no year has, or not in its own
// year (DateRules.h), maps to nothing - {0, 0, 0}; a result past its month's end (an
// unknown-year 30 Adar in a leap year -> "30 Adar II") is the real day it stands for
// (1 Nisan - the same day the arithmetic always gave).
static bool mappable(int targetYear, const HebrewDate& o) {
    return targetYear >= 1 && DateRules::validHebrew(o.year > 0 ? o.year : 0, o.month, o.day);
}
static HebrewDate realDay(const HebrewDate& r) {
    if (DateRules::validHebrew(r.year, r.month, r.day)) return r;
    HebrewDate x;
    JewishDateEngine::absoluteToHebrew(JewishDateEngine::hebrewToAbsolute(r.year, r.month, 1) + r.day - 1, x.year, x.month, x.day);
    return x;
}

HebrewDate JewishDateEngine::resolveBirthdayOrAnniversary(int targetYear, const HebrewDate& original) {
    if (!mappable(targetYear, original)) return HebrewDate{0, 0, 0};
    // Birthdays/weddings/other: a missing 30th always rolls FORWARD to the
    // 1st of the next month, recomputed fresh for every target year.
    if (original.year > 0 && targetYear == original.year) return original;

    // Unknown original year: assume non-leap (the more common case, ~12 of
    // every 19 years) rather than refuse to resolve at all.
    bool wasLeap = (original.year > 0) && isHebrewLeapYear(original.year);
    int month = original.month, day = original.day;

    if ((month == HebrewMonths::ADAR && !wasLeap) || (month == HebrewMonths::ADAR_II && wasLeap)) {
        month = lastMonthOfHebrewYear(targetYear); // promotes to Adar II in a leap target year
    } else if (month == HebrewMonths::CHESHVAN && day == 30 && daysInHebrewMonth(HebrewMonths::CHESHVAN, targetYear) != 30) {
        month = HebrewMonths::KISLEV; day = 1;
    } else if (month == HebrewMonths::KISLEV && day == 30 && daysInHebrewMonth(HebrewMonths::KISLEV, targetYear) == 29) {
        month = HebrewMonths::TEVET; day = 1;
    } else if (month == HebrewMonths::ADAR && day == 30 && wasLeap && !isHebrewLeapYear(targetYear)) {
        month = HebrewMonths::NISSAN; day = 1; // 30 Adar I, target year has only one (29-day) Adar
    }

    HebrewDate result;
    result.year = targetYear; result.month = month; result.day = day;
    return realDay(result);
}

HebrewDate JewishDateEngine::resolveYahrzeit(int targetYear, const HebrewDate& original) {
    if (!mappable(targetYear, original)) return HebrewDate{0, 0, 0};
    int month = original.month, day = original.day;

    // "Locked" by the character of the FIRST anniversary (the year right
    // after death) - once that first anniversary falls on a short month,
    // the yahrzeit is observed on the actual last day of that month every
    // year after (which itself varies 29/30 by year - it is not always 29).
    // Only meaningful when the original year is known; an unknown year (0)
    // has no "first anniversary" to check, so falls through to the simpler
    // per-target-year rule below instead.
    if (original.year > 0) {
        if (month == HebrewMonths::CHESHVAN && day == 30 && daysInHebrewMonth(HebrewMonths::CHESHVAN, original.year + 1) != 30) {
            HebrewDate result;
            absoluteToHebrew(hebrewToAbsolute(targetYear, HebrewMonths::KISLEV, 1) - 1, result.year, result.month, result.day);
            return result;
        }
        if (month == HebrewMonths::KISLEV && day == 30 && daysInHebrewMonth(HebrewMonths::KISLEV, original.year + 1) == 29) {
            HebrewDate result;
            absoluteToHebrew(hebrewToAbsolute(targetYear, HebrewMonths::TEVET, 1) - 1, result.year, result.month, result.day);
            return result;
        }
    }

    if (month == HebrewMonths::ADAR_II) {
        month = lastMonthOfHebrewYear(targetYear); // no promotion for plain Adar - see header comment
    } else if (month == HebrewMonths::ADAR && day == 30 && !isHebrewLeapYear(targetYear)) {
        month = HebrewMonths::SHEVAT; day = 30; // 30 Adar I, target year has only one Adar -> last day of Shevat
    }

    // Still an unresolved 30th (first anniversary wasn't short, or year
    // unknown): roll forward only in years that are themselves short.
    if (month == HebrewMonths::CHESHVAN && day == 30 && daysInHebrewMonth(HebrewMonths::CHESHVAN, targetYear) != 30) {
        month = HebrewMonths::KISLEV; day = 1;
    } else if (month == HebrewMonths::KISLEV && day == 30 && daysInHebrewMonth(HebrewMonths::KISLEV, targetYear) == 29) {
        month = HebrewMonths::TEVET; day = 1;
    }

    HebrewDate result;
    result.year = targetYear; result.month = month; result.day = day;
    return realDay(result);
}

// =============================================================================
// FORMATTING (Pure)
// =============================================================================

// Cheshvan is spelled חשוון (double vav): without nikud, that is the standard
// readable spelling; Hebcal's own חשון relies on its nikud (חֶשְׁוָן). Hebcal
// text gets the same fix in Calendar.cpp (fixMonthSpelling()).
static const char* s_monthNames[] = {
    "", "תשרי", "חשוון", "כסלו", "טבת", "שבט", "אדר",
    "ניסן", "אייר", "סיון", "תמוז", "אב", "אלול", "אדר ב׳"
};

const char* JewishDateEngine::getMonthName(int month) {
    if (month >= 1 && month <= 13) return s_monthNames[month];
    return "Unknown";
}

int JewishDateEngine::getMonthFromName(const char* name) {
    if (!name) return 0;
    for (int i = 1; i <= 13; i++) {
        if (strcmp(name, s_monthNames[i]) == 0) return i;
    }
    // Other spellings still accepted on input (old Excel sheets, an old
    // browser tab still showing the previous page): the earlier single-vav
    // spelling, and the full name with or without the double vav.
    if (strcmp(name, "חשון") == 0 || strcmp(name, "מרחשון") == 0 || strcmp(name, "מרחשוון") == 0) return 2;
    return 0;
}

bool JewishDateEngine::parseHebcalDate(const char* s, HebrewDate& out) {
    // Hebcal's English transliteration; plain "Adar" in a common year, Adar I/II in a leap year.
    static const struct { const char* name; int month; } NAMES[] = {
        {"Tishrei", 1}, {"Cheshvan", 2}, {"Kislev", 3}, {"Tevet", 4}, {"Sh'vat", 5},
        {"Adar I", 6}, {"Adar II", 13}, {"Adar", 6}, {"Nisan", 7}, {"Iyyar", 8},
        {"Sivan", 9}, {"Tamuz", 10}, {"Av", 11}, {"Elul", 12},
    };
    if (!s) return false;
    int day = 0, n = 0;
    if (sscanf(s, "%2d %n", &day, &n) != 1 || n == 0) return false;
    const char* rest = s + n;
    const char* sp = strrchr(rest, ' ');                 // the year is the last word
    if (!sp || sp == rest) return false;
    int year = 0, used = 0;
    if (sscanf(sp + 1, "%4d%n", &year, &used) != 1 || sp[1 + used] != '\0') return false;
    size_t monthLen = (size_t)(sp - rest);
    for (const auto& m : NAMES) {
        if (strlen(m.name) != monthLen || strncmp(rest, m.name, monthLen) != 0) continue;
        if ((m.month == 6 || m.month == 13) && (strcmp(m.name, "Adar") == 0) == isHebrewLeapYear(year)) return false;
        if (!DateRules::validHebrew(year, m.month, day)) return false;
        out = HebrewDate{year, m.month, day};
        return true;
    }
    return false;
}

void JewishDateEngine::numToGematria(int n, char* buf, size_t len) {
    static const char* hundreds[] = {"","ק","ר","ש","ת","תק","תר","תש","תת","תתק"};
    static const char* tens[]     = {"","י","כ","ל","מ","נ","ס","ע","פ","צ"};
    static const char* units[]    = {"","א","ב","ג","ד","ה","ו","ז","ח","ט"};

    // Outside what the letters below spell (1..9999): "?" - hundreds[] has 10
    // entries, and n / 100 would read past it (found by code_check, 2026-11-13;
    // no date reaches it, but this runs on whatever a file or a service gives).
    if (len == 0) return;
    if (n <= 0 || n > 9999) { StringTools::utf8Cut(buf, len, "?", len - 1); return; }

    // Thousands: their own letter + geresh, then the rest with its own
    // gershayim - 5787 -> "ה׳תשפ״ז", 6000 -> "ו׳", 6060 -> "ו׳ס׳". (The
    // gershayim step below used to run over the thousands too: "ו״׳".)
    if (n >= 1000 && n / 1000 <= 9) {
        char rest[32] = "";
        if (n % 1000) numToGematria(n % 1000, rest, sizeof(rest));
        snprintf(buf, len, "%s׳%s", units[n / 1000], rest);
        return;
    }

    char tmp[32] = "";
    if (n >= 100) { strncat(tmp, hundreds[n / 100], sizeof(tmp) - strlen(tmp) - 1); n %= 100; }
    // 15 and 16 are written ט״ו / ט״ז (not יה / יו), also inside larger
    // numbers: 715 -> תשט״ו (it used to come out as תשי״ה).
    if (n == 15 || n == 16) { strncat(tmp, n == 15 ? "טו" : "טז", sizeof(tmp) - strlen(tmp) - 1); n = 0; }
    if (n >= 10)  { strncat(tmp, tens[n / 10],      sizeof(tmp) - strlen(tmp) - 1); n %= 10; }
    if (n >= 1)   { strncat(tmp, units[n],           sizeof(tmp) - strlen(tmp) - 1); }

    size_t tlen = strlen(tmp);
    if (tlen == 0) { StringTools::utf8Cut(buf, len, "?", len - 1); return; }
    if (tlen == 2) { // One Hebrew letter = 2 UTF-8 bytes (not 1 — strlen counts bytes)
        snprintf(buf, len, "%s׳", tmp); // Geresh for single letters
    } else {
        size_t split = strlen(tmp) - 2; // Position before the last character
        char out[32] = "";
        strncpy(out, tmp, split);
        out[split] = '\0';
        strncat(out, "״", sizeof(out) - strlen(out) - 1);
        strncat(out, tmp + split, sizeof(out) - strlen(out) - 1); // Append last character
        StringTools::utf8Cut(buf, len, out, len - 1);
    }
    buf[len - 1] = '\0';
}

void JewishDateEngine::yearToGematria(int year, char* buf, size_t len) {
    // As years are usually written, and everywhere on the clock: without the
    // thousands (5787 -> תשפ״ז, 6060 -> ס׳) - unless nothing else is left
    // (6000 -> ו׳).
    numToGematria(year >= 1000 && year % 1000 ? year % 1000 : year, buf, len);
}

void JewishDateEngine::formatHebrewDate(const HebrewDate& h, char* buf, size_t len, bool includeYear) {
    char dayStr[16];
    numToGematria(h.day, dayStr, sizeof(dayStr));

    const char* mn = "?";
    if (h.month >= 1 && h.month <= 12) {
        mn = s_monthNames[h.month];
        // In a leap year, Month 6 is Adar I
        if (h.month == 6 && h.year > 0 && isHebrewLeapYear(h.year)) mn = "אדר א׳";
    } else if (h.month == 13) {
        mn = s_monthNames[13]; // Adar II
    }

    if (includeYear) {
        char yearStr[24];
        yearToGematria(h.year, yearStr, sizeof(yearStr));
        snprintf(buf, len, "%s %s %s", dayStr, mn, yearStr);
    } else {
        snprintf(buf, len, "%s %s", dayStr, mn);
    }
}

String JewishDateEngine::formatHebrewDate(const HebrewDate& h, bool includeYear) {
    char buf[64];
    formatHebrewDate(h, buf, sizeof(buf), includeYear);
    return String(buf);
}