#pragma once

#include <cstdint>

// A single enumerated verbosity dial, not per-area flags - deliberately
// simple (one setting to remember, not a growing set of independent
// toggles to associate with the right code). RAM-only, resets to the
// default on every reboot - the one tier that matters for cost (DEBUG)
// is meant to be turned up only while actively investigating something,
// never left on by accident.
//
// Ordering is ascending severity->verbosity, each level showing everything
// at its own tier and below: NONE shows nothing, ERROR shows only errors,
// WARN adds warnings, INFO (the default) adds routine informational
// events, DEBUG adds everything, including expensive per-item traces
// that would otherwise cost real time/Serial bandwidth on every call.
enum class LogLevel : uint8_t {
    NONE  = 0,
    ERROR = 1,
    WARN  = 2,
    INFO  = 3,
    DEBUG = 4
};

namespace Logging {
    void setLevel(LogLevel level);
    LogLevel getLevel();
    const char* levelName(LogLevel level);
    // Case-insensitive; returns `fallback` (not NONE) if name doesn't match
    // one of "none"/"error"/"warn"/"info"/"debug", so a bad request can't
    // silently go quiet.
    LogLevel parseLevelName(const char* name, LogLevel fallback);

    // True if something at this level would currently be shown/recorded -
    // check before doing any work (e.g. serializing a whole JSON item)
    // that's only needed to build the log line itself.
    inline bool enabled(LogLevel level) { return (uint8_t)level <= (uint8_t)getLevel(); }
}

// Routes through DiagnosticLog::record() (prints live AND persists) only
// if `lvl` is at or below the current level - for failure/event logging
// that's worth keeping after a reboot, gated so a failure mode that could
// repeat often (e.g. a sustained DNS outage) doesn't have to flood the
// persistent log at the default level. CLOCK_LOG (StringTools.h) is the
// other half of this system - implicitly INFO level, print-only, never
// persisted (most of its call sites fire every update() tick; persisting
// those would flood the 32KB cap). Use LOG_AT specifically for the minority
// of calls that need a level other than INFO, or that are worth keeping
// after a reboot (see CLAUDE.md's DiagnosticLog coverage note).
#define LOG_AT(lvl, category, fmt, ...) \
    do { if (Logging::enabled(lvl)) DiagnosticLog::record(category, fmt, ##__VA_ARGS__); } while (0)
