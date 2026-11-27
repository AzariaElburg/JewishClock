#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Which version's data a version can read (agreed 2026-11-13) - the one place:
//   version F reads data made by version D when D is not newer than F and at most
//   one major version older (F 3.x reads 2.* and 3.(<= its own)).
// Used by every way data comes in: an update (the new version must read the
// clock's data - Updater.cpp), a backup restore and an Excel import (the file
// says which version made it - Storage::importBackup, /api/events/import).
// A file that names no version (every export before 2026-11-15): read only while
// ACCEPT_UNSTAMPED - the user, 2026-11-15: refused from the second release after the
// first one that stamps its files; by then every file worth reading names its version
// and older ones are too old anyway. Release checklist (CLAUDE.md): then REMOVE the flag and
// every path that reads an unstamped file (the user, 2026-11-19: no dead switch kept).
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <stdio.h>
#include "Texts.h"

namespace VersionRule {

// === TUNABLES ===
static constexpr bool ACCEPT_UNSTAMPED = true;   // removed (with its code) at the 2nd release after the first stamping one

// "2.3.0" / "2.2.1-4-g1a2b3c4" (a.b.c, then commits after the tag) -> 4 numbers.
inline bool parse(const String& v, long out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    int a = 0, b = 0, c = 0, n = 0;
    int got = sscanf(v.c_str(), "%5d.%5d.%5d-%6d", &a, &b, &c, &n);
    out[0] = a; out[1] = b; out[2] = c; out[3] = got == 4 ? n : 0;
    return got >= 3;
}

// candidate newer than running? Unreadable versions: false.
inline bool newer(const String& candidate, const String& running) {
    long x[4], y[4];
    if (!parse(candidate, x) || !parse(running, y)) return false;
    for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

inline long major(const String& v) { long x[4]; return parse(v, x) ? x[0] : -1; }

// Why `firmware` can't read data made by `madeBy` ("" = it can). `what` names the data
// for people ("This backup", "The clock's data").
inline String dataProblem(const String& firmware, const String& madeBy, const char* what) {
    if (!madeBy.length())   // from before files said their version (2.x)
        return ACCEPT_UNSTAMPED ? "" : Texts::fill(Texts::verUnstamped, what, firmware);
    long d[4], f[4];
    if (!parse(madeBy, d)) return Texts::fill(Texts::verUnknown, what, madeBy);
    if (!parse(firmware, f)) return "";
    if (newer(madeBy, firmware))
        return Texts::fill(Texts::verNewer, what, madeBy, firmware);
    if (d[0] < f[0] - 1)
        return Texts::fill(Texts::verTooOld, { what, madeBy, firmware, String(f[0] - 1), String(f[0] - 1) });
    return "";
}

}  // namespace VersionRule
