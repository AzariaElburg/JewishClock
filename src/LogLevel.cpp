#include "LogLevel.h"
#include <cstring>
#include <cstdlib>
#include <atomic>

namespace {
// Not lock-guarded: a single byte, checked on essentially every log call
// from any task. A torn/stale read here costs at most one log line
// showing or not showing around the exact moment the level changes - no
// safety consequence, so a mutex on the hot path isn't worth paying for.
LogLevel s_level = LogLevel::INFO;
std::atomic<uint8_t> s_focus{0};   // LogLevel.h Focus bits - set from the main loop, the web task and the download task
}

void Logging::setLevel(LogLevel level) { s_level = level; }
LogLevel Logging::getLevel() { return s_level; }
uint8_t Logging::focus() { return s_focus.load(std::memory_order_relaxed); }
bool Logging::setFocus(uint8_t area, bool on) {
    uint8_t before = on ? s_focus.fetch_or(area) : s_focus.fetch_and((uint8_t)~area);
    uint8_t after = on ? (uint8_t)(before | area) : (uint8_t)(before & ~area);
    return (before != 0) != (after != 0);
}

const char* Logging::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::NONE:  return "none";
        case LogLevel::ERROR: return "error";
        case LogLevel::WARN:  return "warn";
        case LogLevel::INFO:  return "info";
        case LogLevel::DEBUG: return "debug";
        default:              return "info";
    }
}

LogLevel Logging::parseLevelName(const char* name, LogLevel fallback) {
    if (!name) return fallback;
    if (!strcasecmp(name, "none"))  return LogLevel::NONE;
    if (!strcasecmp(name, "error")) return LogLevel::ERROR;
    if (!strcasecmp(name, "warn"))  return LogLevel::WARN;
    if (!strcasecmp(name, "info"))  return LogLevel::INFO;
    if (!strcasecmp(name, "debug")) return LogLevel::DEBUG;
    // Numeric fallback (0-4), for a simple <select> that posts the index.
    char* end = nullptr;
    long n = strtol(name, &end, 10);
    if (end && *end == '\0' && n >= 0 && n <= 4) return (LogLevel)n;
    return fallback;
}
