#pragma once
#include <Arduino.h>
#include "LogLevel.h"
#include "DebugTime.h"

namespace StringTools {
    // Strips non-alphanumeric chars and capitalizes the start of each word,
    // preserving existing case elsewhere so embedded CamelCase (e.g. "JewishClock")
    // survives round-tripping instead of being flattened to "Jewishclock".
    inline String normalize(const String& raw, const char* fallback = "JewishClock") {
        String out;
        bool nextUpper = true;
        for (size_t i = 0; i < raw.length(); i++) {
            char c = raw[i];
            if (isalnum(c)) {
                if (nextUpper) { out += (char)toupper(c); nextUpper = false; }
                else { out += c; }
            } else if (isspace(c) || c == '-' || c == '_' || c == '.') {
                nextUpper = true;
            }
        }
        return (out.length() == 0) ? String(fallback) : out;
    }

    // Splits CamelCase into "Camel Case" (or any other separator)
    inline String formatCamelCase(const String& camel, const char* separator = " ") {
        String out;
        for (size_t i = 0; i < camel.length(); i++) {
            char c = camel[i];
            if (i > 0 && isupper(c) && islower(camel[i - 1])) {
                out += separator;
            }
            out += c;
        }
        return out;
    }

    // Helper for formatting the project name for display
    inline String formatProjectName(const char* raw) {
        return formatCamelCase(normalize(String(raw)));
    }

    // Generates a valid mDNS hostname (lowercase, alphanumeric + dashes, max 63 chars)
    inline String makeMdnsHostname(const String& raw) {
        String out = normalize(raw, "jewishclock");
        out.toLowerCase();
        if (out.length() > 63) {
            out = out.substring(0, 63);
        }
        return out;
    }

    // Central build timestamp
    inline String getBuildTimestamp() {
        return String(__DATE__) + " " + String(__TIME__);
    }

    // Returns the local time the firmware is acting on - debug offset included
    // (see DebugTime.h) - or uptime if not synced, as hh:mm:ss.000 for logging.
    inline String getTimestamp() {
        char buf[20];
        time_t now = debugAdjustedNow();
        uint32_t ms = millis() % 1000;

        if (now == 0) { // Not synced yet
            uint32_t m = millis();
            snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", 
                (unsigned int)(m / 3600000), (unsigned int)((m / 60000) % 60), 
                (unsigned int)((m / 1000) % 60), (unsigned int)ms);
        } else {
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03u", 
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, (unsigned int)ms);
        }
        return String(buf);
    }

    // Formats seconds into hours (float), showing decimals only if needed
    inline String formatOffsetHours(int seconds) {
        float hours = seconds / 3600.0f;
        if (hours == (int)hours) return String((int)hours);
        return String(hours, 1);
    }
}

// Centralized tracing macro: adds timestamp and tag, handles variadic
// arguments. Implicitly INFO level - the vast majority of call sites never
// said a level at all, and retrofitting one onto every single one of them
// would be a huge, low-value effort (and the same "too much overhead"
// argument against per-area debug flags applies here too). Gating on INFO
// here means: at the default level (INFO) this is byte-for-byte what it
// always was - everything still prints. Turning the dial down to WARN/
// ERROR/NONE now actually quiets this ambient tracing too, instead of only
// affecting the handful of explicit LOG_AT() call sites. Never persists to
// DiagnosticLog regardless of level (unlike LOG_AT) - most of these call
// sites fire every update() tick; persisting them would flood the 32KB cap
// in minutes. Reserve LOG_AT() for the minority of calls that are actually
// worth a level OTHER than INFO (a failure worth persisting, or detail too
// expensive to compute at the default level) - CLOCK_LOG needs no change at
// those sites, callers already wrap the rare DEBUG-only ones in their own
// `if (Logging::enabled(LogLevel::DEBUG))` check.
#define CLOCK_LOG(tag, fmt, ...) \
    do { if (Logging::enabled(LogLevel::INFO)) Serial.printf("[%s] [%s] " fmt "\n", StringTools::getTimestamp().c_str(), tag, ##__VA_ARGS__); } while (0)
