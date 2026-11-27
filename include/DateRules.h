#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DateRules: what a date or a time may be - the ONE place (2026-11-14).
//
// Every date that comes in - typed, imported, restored, the clock's own files,
// Hebcal's answers, the calendar cache, the conversion route, debug time - is
// checked here, and every event date that isn't in every year is mapped here
// (or in JewishDateEngine's Hebcal-ported rules, which this calls). A fix or a
// refinement here reaches all of them.
//
// The rules (the user, 2026-11-14):
//   - 30 February, 31 April, 31 Tevet ...: never a date.
//   - 29 February, 30 Cheshvan, 30 Kislev, 30 Adar I, Adar II: dates only some
//     years have. Without a year: accepted (a birthday may have no year). With
//     a year: only if that year has the day.
//   - An event on such a day, in a year without it, is shown (and its
//     "days before") on the day the rules map it to: 29 Feb -> 1 Mar
//     (resolveGregorian(); as gregorianToAbsolute() always did); the Hebrew
//     ones -> JewishDateEngine::resolveBirthdayOrAnniversary() / resolveYahrzeit().
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <stdio.h>
#include "JewishDateEngine.h"
#include "Texts.h"

namespace DateRules {

inline bool gregorianLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

// Days in a Gregorian month; year 0 = "any year" (February: 29). 0 = no such month.
inline int gregorianMonthDays(int y, int m) {
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && (y == 0 || gregorianLeap(y))) return 29;
    return days[m];
}

// Days in a Hebrew month (1 = Tishrei ... 6 = Adar / Adar I, 13 = Adar II);
// year 0 = "any year": the most it ever has. 0 = no such month (or Adar II in
// a year without it).
inline int hebrewMonthDays(int y, int m) {
    if (m < 1 || m > 13) return 0;
    if (y == 0) {
        static const int most[] = {0, 30, 30, 30, 29, 30, 30, 30, 29, 30, 29, 30, 29, 29};
        return most[m];
    }
    if (m == HebrewMonths::ADAR_II && !JewishDateEngine::isHebrewLeapYear(y)) return 0;
    return JewishDateEngine::daysInHebrewMonth(m, y);
}

inline bool validGregorian(int y, int m, int d) { return y >= 0 && d >= 1 && d <= gregorianMonthDays(y, m); }
inline bool validHebrew(int y, int m, int d)    { return y >= 0 && d >= 1 && d <= hebrewMonthDays(y, m); }
inline bool validTime(int h, int mi, int s = 0) { return h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && s >= 0 && s <= 60; }

// What's wrong with a date, for people ("" = nothing). Names the month and,
// for a day only some years have, the year that lacks it. *fields: which of
// the numbers ("m", "d", "d y", "m y") - the page names those Excel columns.
inline String gregorianProblem(int y, int m, int d, const char** fields = nullptr) {
    static const char* const names[] = {"", "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December"};
    const char* f = "";
    String t;
    if (m < 1 || m > 12) { f = "m"; t = Texts::fill(Texts::gMonthRange, String(m)); }
    else if (d < 1 || d > gregorianMonthDays(0, m))
        { f = "d"; t = Texts::fill(Texts::noDay, names[m], String(d), m == 2 ? String(Texts::febDays) : Texts::fill(Texts::nDays, String(gregorianMonthDays(0, m)))); }
    else if (!validGregorian(y, m, d)) { f = "d y"; t = Texts::fill(Texts::febNot29, String(y), String(y)); }
    if (fields) *fields = f;
    return t;
}
inline String hebrewProblem(int y, int m, int d, const char** fields = nullptr) {
    String t;
    const char* f = "";
    if (fields) *fields = "";
    if (m < 1 || m > 13) { if (fields) *fields = "m"; return Texts::fill(Texts::hMonthRange, String(m)); }
    static const char* const names[] = {"", "Tishrei", "Cheshvan", "Kislev", "Tevet", "Shevat", "Adar", "Nisan",
                                        "Iyar", "Sivan", "Tammuz", "Av", "Elul", "Adar II"};
    const char* name = names[m];
    if (d < 1 || d > hebrewMonthDays(0, m)) {
        f = "d";
        const bool varies = m == HebrewMonths::CHESHVAN || m == HebrewMonths::KISLEV || m == HebrewMonths::ADAR;
        t = Texts::fill(Texts::noDay, name, String(d), varies ? String(Texts::hDaysVary) : Texts::fill(Texts::nDays, String(hebrewMonthDays(0, m))));
    }
    else if (validHebrew(y, m, d)) return "";
    else if (m == HebrewMonths::ADAR_II) { f = "m y"; t = Texts::fill(Texts::noAdarII, String(y)); }
    else { f = "d y"; t = Texts::fill(Texts::hYearDays, { name, String(y), String(hebrewMonthDays(y, m)), String(d) }); }
    if (fields) *fields = f;
    return t;
}

// "YYYY-MM-DD..." (anything after the date ignored): true only for a real date.
inline bool parseIsoDate(const char* s, int& y, int& m, int& d) {
    if (!s || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
    return y >= 1 && validGregorian(y, m, d);
}
// "YYYY-MM-DDTHH:MM[:SS]...": true only for a real date and time (seconds 0 if left out).
inline bool parseIsoDateTime(const char* s, int& y, int& m, int& d, int& h, int& mi, int& sec) {
    sec = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &m, &d, &h, &mi, &sec) < 5) return false;
    return y >= 1 && validGregorian(y, m, d) && validTime(h, mi, sec);
}
// "HH:MM[:SS]": true only for a real time.
inline bool parseTime(const char* s, int& h, int& mi, int& sec) {
    sec = 0;
    if (!s || sscanf(s, "%2d:%2d:%2d", &h, &mi, &sec) < 2) return false;
    return validTime(h, mi, sec);
}

// The day (absolute, JewishDateEngine's count) an event on month/day falls on
// in `year`: 29 February in a year without it -> 1 March. Checks its own input
// (no counting on the caller): a month/day no year has -> RESOLVE_NONE.
static constexpr long RESOLVE_NONE = -1;
inline long resolveGregorian(int year, int m, int d) {
    if (year < 1 || !validGregorian(0, m, d)) return RESOLVE_NONE;
    if (m == 2 && d == 29 && !gregorianLeap(year)) { m = 3; d = 1; }
    return JewishDateEngine::gregorianToAbsolute(year, m, d);
}

} // namespace DateRules
