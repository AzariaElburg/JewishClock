#pragma once

#include <Arduino.h>

// ─── Stateless Data Models ──────────────────────────────────────────────────

struct HebrewDate {
    int year;   // 0 for repeating/unknown
    int month;  // 1-13 (1=Tishrei, 6=Adar, 7=Nissan, 13=Adar II)
    int day;    // 1-30
};

struct GregorianDate {
    int year;   // 0 for repeating/unknown
    int month;  // 1-12
    int day;    // 1-31
    bool isAfterSunset; // The anchor for Hebrew day transition
};

// ─── JewishDateEngine (Stateless Calculator) ────────────────────────────────

class JewishDateEngine {
public:
    // Core Conversions (Pure)
    static HebrewDate    calculateHebrew(const GregorianDate& g);
    static GregorianDate calculateGregorian(const HebrewDate& h);
    
    // Validation (Pure)
    // Returns true if the two dates can refer to the same physical day.
    // Accounts for the "Sliding Overlap" via +/- 1 day check.
    static bool isValidCombination(const GregorianDate& g, const HebrewDate& h, String& errorMsg);

    // Formatting (Pure)
    static void formatHebrewDate(const HebrewDate& h, char* buf, size_t len, bool includeYear = true);
    static String formatHebrewDate(const HebrewDate& h, bool includeYear = true);
    static void numToGematria(int n, char* buf, size_t len);
    static void yearToGematria(int year, char* buf, size_t len);   // without the thousands, as shown on the clock

    // Month Name Helpers (Pure)
    static const char* getMonthName(int month);
    static int getMonthFromName(const char* name);

    // Low-level Math (Pure - Based on Dershowitz & Reingold)
    static bool isHebrewLeapYear(int year);
    static int  daysInHebrewMonth(int month, int year);
    static int  daysInHebrewYear(int year);
    
    static long gregorianToAbsolute(int y, int m, int d);
    static void absoluteToGregorian(long abs, int& y, int& m, int& d);
    static long hebrewToAbsolute(int y, int m, int d);
    static void absoluteToHebrew(long abs, int& y, int& m, int& d);

    // Personal-event recurrence (birthdays/weddings/other vs. yahrzeits) across
    // the Hebrew calendar's two quirks: Cheshvan/Kislev are each 29 or 30 days
    // depending on the year, and leap years have two Adars. Ported from
    // Hebcal's own anniversary.go - see JewishDateEngine.cpp for full credit
    // and the source link. "original" may have year <= 0 (unknown year); the
    // functions degrade gracefully in that case, see .cpp comments.
    static HebrewDate resolveBirthdayOrAnniversary(int targetYear, const HebrewDate& original);
    static HebrewDate resolveYahrzeit(int targetYear, const HebrewDate& original);

private:
    static long hebrewElapsedDays(int y);
};

// Hebrew Month Constants (Civil/Religious order: Tishrei=1)
namespace HebrewMonths {
    const int TISHREI   = 1;
    const int CHESHVAN  = 2;
    const int KISLEV    = 3;
    const int TEVET     = 4;
    const int SHEVAT    = 5;
    const int ADAR      = 6;
    const int NISSAN    = 7;
    const int IYAR      = 8;
    const int SIVAN     = 9;
    const int TAMMUZ    = 10;
    const int AV        = 11;
    const int ELUL      = 12;
    const int ADAR_II   = 13;
}