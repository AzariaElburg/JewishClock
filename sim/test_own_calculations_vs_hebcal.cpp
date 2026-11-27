// The clock's own calculations against Hebcal's real answers (sim/hebcal_reference.json,
// made by sim/make_hebcal_reference.py): the Hebrew date of every day of 21 Hebrew years
// (JewishDateEngine - dates every event, and today when there is no download), sunset and
// צאת הכוכבים for 8 extreme places (SunTimes - used when Hebcal has no time for today),
// the reading of Hebcal's own date text (parseHebcalDate - what the clock now uses), and 25 years of
// yahrzeits / Hebrew birthdays for the hard cases (30 Cheshvan, 30 Kislev, Adar I/II).
// Built and run by sim/test_own_calculations_vs_hebcal.py.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "JewishDateEngine.h"
#include "SunTimes.h"

static int g_checks = 0, g_failed = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) { g_failed++; if (g_failed <= 40) printf("FAILED: %s\n", what); }
}

static long absOf(const char* iso) {
    int y, m, d;
    sscanf(iso, "%d-%d-%d", &y, &m, &d);
    return JewishDateEngine::gregorianToAbsolute(y, m, d);
}
static GregorianDate gregOf(long abs) {
    // Gregorian date of an absolute day: from a year's start, walked forward (a few hundred steps at most).
    int y = (int)(abs / 365.2425) + 1;
    while (JewishDateEngine::gregorianToAbsolute(y + 1, 1, 1) <= abs) y++;
    while (JewishDateEngine::gregorianToAbsolute(y, 1, 1) > abs) y--;
    int m = 1;
    while (m < 12 && JewishDateEngine::gregorianToAbsolute(y, m + 1, 1) <= abs) m++;
    return GregorianDate{y, m, (int)(abs - JewishDateEngine::gregorianToAbsolute(y, m, 1)) + 1, false};
}

static void hebrewDates(JsonObject years) {
    long days = 0;
    for (JsonPair yp : years) {
        int hy = atoi(yp.key().c_str());
        JsonArray starts = yp.value().as<JsonArray>();
        for (size_t i = 0; i < starts.size(); i++) {
            int month = starts[i][0];
            long first = absOf(starts[i][1]);
            // The month runs to the next month's first day; Elul (the year's last) is always 29 days.
            long end = (i + 1 < starts.size()) ? absOf(starts[i + 1][1]) : first + 29;
            char what[160];
            snprintf(what, sizeof(what), "1 of month %d of %d = %s (Hebcal) back from Hebrew", month, hy, (const char*)starts[i][1]);
            GregorianDate back = JewishDateEngine::calculateGregorian(HebrewDate{hy, month, 1});
            check(JewishDateEngine::gregorianToAbsolute(back.year, back.month, back.day) == first, what);
            for (long a = first; a < end; a++, days++) {
                HebrewDate h = JewishDateEngine::calculateHebrew(gregOf(a));
                int want = (int)(a - first) + 1;
                snprintf(what, sizeof(what), "%d/%d/%d (Hebcal) - own %d/%d/%d", want, month, hy, h.day, h.month, h.year);
                check(h.year == hy && h.month == month && h.day == want, what);
            }
        }
        check(starts.size() == (JewishDateEngine::isHebrewLeapYear(hy) ? 13u : 12u), "months in a year (leap year rule)");
    }
    printf("Hebrew dates: %ld days of %u years\n", days, (unsigned)years.size());
}

static void hebcalText(JsonObject years) {
    struct { const char* s; bool ok; int y, m, d; } cases[] = {
        {"20 Tevet 5787", true, 5787, 4, 20},     {"1 Sh'vat 5786", true, 5786, 5, 1},
        {"1 Adar I 5787", true, 5787, 6, 1},      {"30 Adar I 5787", true, 5787, 6, 30},
        {"1 Adar II 5787", true, 5787, 13, 1},    {"29 Adar 5786", true, 5786, 6, 29},
        {"1 Adar 5787", false, 0, 0, 0},          // plain Adar in a leap year
        {"1 Adar I 5786", false, 0, 0, 0},        // Adar I in a common year
        {"1 Adar II 5786", false, 0, 0, 0},       {"30 Adar 5786", false, 0, 0, 0},
        {"1 Tishrei 6000", true, 6000, 1, 1},     {"29 Elul 5999", true, 5999, 12, 29},
        {"30 Elul 5786", false, 0, 0, 0},         {"0 Tevet 5787", false, 0, 0, 0},
        {"31 Tevet 5787", false, 0, 0, 0},        {"20 Teveth 5787", false, 0, 0, 0},
        {"Tevet 5787", false, 0, 0, 0},           {"20 Tevet", false, 0, 0, 0},
        {"20 Tevet 5787x", false, 0, 0, 0},       {"", false, 0, 0, 0},
    };
    for (auto& c : cases) {
        HebrewDate h{0, 0, 0};
        bool ok = JewishDateEngine::parseHebcalDate(c.s, h);
        char what[96];
        snprintf(what, sizeof(what), "Hebcal text \"%s\" -> %s", c.s, c.ok ? "read" : "refused");
        check(ok == c.ok && (!ok || (h.year == c.y && h.month == c.m && h.day == c.d)), what);
    }
    // 30 Cheshvan / 30 Kislev: read only in a year that has it - the lengths from Hebcal's own month starts.
    int checked = 0;
    for (JsonPair yp : years) {
        int hy = atoi(yp.key().c_str());
        JsonArray s = yp.value().as<JsonArray>();
        for (int k = 1; k <= 2 && k + 1 < (int)s.size(); k++) {   // [1] Cheshvan, [2] Kislev
            bool has30 = absOf(s[k + 1][1]) - absOf(s[k][1]) == 30;
            char text[40], what[96];
            snprintf(text, sizeof(text), "30 %s %d", k == 1 ? "Cheshvan" : "Kislev", hy);
            HebrewDate h;
            snprintf(what, sizeof(what), "Hebcal text \"%s\" -> %s", text, has30 ? "read" : "refused");
            check(JewishDateEngine::parseHebcalDate(text, h) == has30, what);
            checked++;
        }
    }
    printf("Hebcal date texts: %u fixed + %d 30th-day cases\n", (unsigned)(sizeof(cases) / sizeof(cases[0])), checked);
}

static void zmanim(JsonArray places) {
    int n = 0, none = 0;
    double worst = 0;
    for (JsonObject p : places) {
        double lat = p["lat"], lon = p["lon"];
        for (JsonArray row : p["days"].as<JsonArray>()) {
            int y, m, d;
            sscanf(row[0].as<const char*>(), "%d-%d-%d", &y, &m, &d);
            for (int k = 0; k < 2; k++) {
                double mine = 0;
                bool ok = SunTimes::eveningUtcMin(y, m, d, lat, lon, k ? SunTimes::TZEIT_DEG : SunTimes::SUNSET_DEG, mine);
                char what[160];
                if (row[1 + k].isNull()) {
                    none++;
                    snprintf(what, sizeof(what), "%s %s %s: Hebcal has none, own %s", (const char*)p["place"], (const char*)row[0],
                             k ? "tzeit" : "sunset", ok ? "has one" : "none");
                    check(!ok, what);
                } else {
                    double ref = row[1 + k].as<double>();
                    double diff = ok ? fabs(mine - ref) : 999;
                    if (ok && diff > worst) worst = diff;
                    snprintf(what, sizeof(what), "%s %s %s: Hebcal %.0f min UTC, own %.1f", (const char*)p["place"], (const char*)row[0],
                             k ? "tzeit" : "sunset", ref, ok ? mine : -1.0);
                    check(diff <= 1.0, what);   // Hebcal's times are whole minutes
                }
                n++;
            }
        }
    }
    printf("Sunset/tzeit: %d times of %u places (%d where the sun doesn't get that low), worst %.2f min\n",
           n, (unsigned)places.size(), none, worst);
}

// Yahrzeits and Hebrew birthdays (resolveYahrzeit / resolveBirthdayOrAnniversary - ported from
// Hebcal) against Hebcal's own yahrzeit API, 25 years of each hard case (2026-11-18).
static void anniversaries(JsonArray cases) {
    int n = 0;
    for (JsonObject c : cases) {
        int gy, gm, gd;
        sscanf(c["g"].as<const char*>(), "%d-%d-%d", &gy, &gm, &gd);
        HebrewDate orig = JewishDateEngine::calculateHebrew(GregorianDate{gy, gm, gd, false});
        bool yahrzeit = strcmp(c["type"] | "", "Yahrzeit") == 0;
        for (JsonArray d : c["dates"].as<JsonArray>()) {
            HebrewDate want{0, 0, 0};
            JewishDateEngine::parseHebcalDate(d[1].as<const char*>(), want);
            HebrewDate mine = yahrzeit ? JewishDateEngine::resolveYahrzeit(want.year, orig)
                                       : JewishDateEngine::resolveBirthdayOrAnniversary(want.year, orig);
            GregorianDate g = JewishDateEngine::calculateGregorian(mine);
            char gs[16], what[200];
            snprintf(gs, sizeof(gs), "%04d-%02d-%02d", g.year, g.month, g.day);
            snprintf(what, sizeof(what), "%s of %s (%s) in %d: Hebcal %s = %s, own %d/%d/%d = %s",
                     c["type"].as<const char*>(), c["case"].as<const char*>(), c["g"].as<const char*>(), want.year,
                     d[1].as<const char*>(), d[0].as<const char*>(), mine.day, mine.month, mine.year, gs);
            check(want.year > 0 && mine.year == want.year && mine.month == want.month && mine.day == want.day && strcmp(gs, d[0]) == 0, what);
            n++;
        }
    }
    printf("Yahrzeits / Hebrew birthdays: %d dates of %u cases\n", n, (unsigned)cases.size());
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_own_calculations_vs_hebcal REFERENCE.json\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("no %s\n", argv[1]); return 2; }
    std::string text;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    fclose(f);
    JsonDocument doc;
    if (deserializeJson(doc, text.c_str(), text.size())) { printf("reference not readable\n"); return 2; }
    hebrewDates(doc["hebrewYears"].as<JsonObject>());
    hebcalText(doc["hebrewYears"].as<JsonObject>());
    zmanim(doc["zmanim"].as<JsonArray>());
    anniversaries(doc["anniversaries"].as<JsonArray>());
    printf("== %d checks, %d failed ==\n", g_checks, g_failed);
    if (!g_failed) printf("own calculations vs Hebcal test: ALL CORRECT\n");
    return g_failed ? 1 : 0;
}
