#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Tunables something OUTSIDE their file depends on - a test, a script - are
// served by the clock (GET /api/tunables; jcsim --tunables), so nobody keeps
// a copy that can drift (the user, 2026-11-19). The value stays defined once,
// in its own file's TUNABLES block (CLAUDE.md "Where constants go"); that file
// adds one line below it per value others need:
//     TUNABLE_SHARED(FAILED_RETRY_MS)
// Names are the code's own names, unique across files (a second one with the
// same name is reported by /api/tunables as "duplicates").
// ─────────────────────────────────────────────────────────────────────────────
#include <ArduinoJson.h>

namespace Tunables {

struct Entry {
    const char* name;
    double      number;
    const char* text;      // a text tunable (a message), else nullptr
    Entry*      next;
};

void add(Entry* e) noexcept;        // called by Reg's constructor, before setup() - never throws (a list link)
void toJson(JsonDocument& doc);     // {"NAME": value, ...} (+ "duplicates": [...] if any)
double number(const char* name);    // one by name, for a test built with the clock's code (NAN: none)

struct Reg {
    Entry e;
    Reg(const char* n, double v) noexcept      : e{n, v, nullptr, nullptr} { add(&e); }
    Reg(const char* n, const char* t) noexcept : e{n, 0, t, nullptr}       { add(&e); }
};

}  // namespace Tunables

#define TUNABLE_SHARED(name) static Tunables::Reg tunableReg_##name(#name, name);
// One that lives in a class or namespace: TUNABLE_SHARED_AS(BOOT_OVERLAY_MS, MessageAggregator::BOOT_OVERLAY_MS)
#define TUNABLE_SHARED_AS(name, value) static Tunables::Reg tunableReg_##name(#name, value);
