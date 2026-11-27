#pragma once
#include <Arduino.h>
#include <cstdarg>
#include "LogLevel.h"
#include "DebugTime.h"
#include "Uptime.h"

namespace StringTools {
    // ASCII character classes - never the C library's isdigit()/isalnum()/toupper()... (2026-11-21:
    // given a plain char, a Hebrew letter's bytes are negative there - undefined behaviour, and the
    // clock's name could keep half a letter). Bytes above 127 are never a letter or digit here.
    // scripts/check_rules.py stops a build with the library ones anywhere else.
    inline bool asciiDigit(char c)  { return c >= '0' && c <= '9'; }
    inline bool asciiUpper(char c)  { return c >= 'A' && c <= 'Z'; }
    inline bool asciiLower(char c)  { return c >= 'a' && c <= 'z'; }
    inline bool asciiAlpha(char c)  { return asciiUpper(c) || asciiLower(c); }
    inline bool asciiAlnum(char c)  { return asciiAlpha(c) || asciiDigit(c); }
    inline bool asciiHex(char c)    { return asciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    inline bool asciiSpace(char c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
    inline char asciiToUpper(char c) { return asciiLower(c) ? (char)(c - 'a' + 'A') : c; }
    inline char asciiToLower(char c) { return asciiUpper(c) ? (char)(c - 'A' + 'a') : c; }
    // Only what every keyboard and every router takes: English letters, digits, plain signs and the
    // space (32-126). Wi-Fi names and passwords (the user, 2026-11-21: "we do not know what the router
    // supports - stick to the safe and sure"), the clock's own Wi-Fi password.
    inline bool asciiPlain(const char* s) {
        for (; s && *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return false;
        return true;
    }

    // Strips non-alphanumeric chars and capitalizes the start of each word,
    // preserving existing case elsewhere so embedded CamelCase (e.g. "JewishClock")
    // survives round-tripping instead of being flattened to "Jewishclock".
    inline String normalize(const String& raw, const char* fallback = "JewishClock") {
        String out;
        bool nextUpper = true;
        for (size_t i = 0; i < raw.length(); i++) {
            char c = raw[i];
            if (asciiAlnum(c)) {
                if (nextUpper) { out += asciiToUpper(c); nextUpper = false; }
                else { out += c; }
            } else if (asciiSpace(c) || c == '-' || c == '_' || c == '.') {
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
            if (i > 0 && asciiUpper(c) && asciiLower(camel[i - 1])) {
                out += separator;
            }
            out += c;
        }
        return out;
    }

    // dst = src, at most maxBytes (and what dst holds), never cut inside a UTF-8
    // letter - and a letter already cut short at the end (an earlier fixed-size
    // copy, e.g. strlcpy into the field) is dropped too. True if it changed anything.
    inline bool utf8Cut(char* dst, size_t dstSize, const char* src, size_t maxBytes) {
        size_t len = strlen(src), n = len;
        if (maxBytes > dstSize - 1) maxBytes = dstSize - 1;
        if (n > maxBytes) n = maxBytes;
        // The last letter's first byte, and how many bytes that letter needs
        size_t lead = n;
        while (lead > 0 && n - lead < 4 && ((uint8_t)src[lead - 1] & 0xC0) == 0x80) lead--;
        if (lead > 0) {
            uint8_t b = (uint8_t)src[lead - 1];
            size_t need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            if (lead - 1 + need > n) n = lead - 1;   // that letter doesn't fit whole: leave it out
        }
        memmove(dst, src, n);
        dst[n] = '\0';
        return n != len;
    }
    // The everyday case: fill a char[] field with as much of src as it holds,
    // whole letters only - use it instead of strlcpy/strncpy for any text that
    // may hold more than plain ASCII (names, Hebrew from the services, icons).
    template <size_t N>
    inline bool utf8Copy(char (&dst)[N], const char* src) { return utf8Cut(dst, N, src ? src : "", N - 1); }

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
        uint32_t ms = (uint32_t)(Uptime::ms() % 1000);

        if (now == 0) { // Not synced yet
            int64_t m = Uptime::ms();
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

    // CLOCK_LOGF's line: "[time] [tag] message", one write
    inline void clockLogf(const char* tag, const char* fmt, ...) {
        char msg[400];
        va_list a;
        va_start(a, fmt);
        vsnprintf(msg, sizeof(msg), fmt, a);
        va_end(a);
        Serial.printf("[%s] [%s] %s\n", getTimestamp().c_str(), tag, msg);
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
// The same line, its format from the list of the clock's words (Texts.h LOG - a named format
// can't be glued in at compile time like CLOCK_LOG's). Built in one piece: lines from other
// tasks can't land in the middle.
#define CLOCK_LOGF(tag, fmt, ...) \
    do { if (Logging::enabled(LogLevel::INFO)) StringTools::clockLogf(tag, fmt, ##__VA_ARGS__); } while (0)
