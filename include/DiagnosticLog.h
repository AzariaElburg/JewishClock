#pragma once

#include <Arduino.h>

class AsyncWebServerRequest; // fwd decl only - keeps ESPAsyncWebServer out of this header

// Small, bounded, persistent event log for things worth correlating after the
// fact (WiFi reconnects, mDNS reinit attempts, boot events, etc.) - NOT a
// replacement for CLOCK_LOG. record() always does both: prints live via
// CLOCK_LOG and appends a line to LittleFS, so the live serial view and the
// persisted history never diverge. Generic by design - not tied to any one
// subsystem; category is just a short tag ("WIFI", "MDNS", "BOOT", "NOTE").
//
// Survives reboots, but NOT `pio run -t uploadfs` (that replaces the whole
// LittleFS partition - see CLAUDE.md's Build & Flash section for the
// mandatory fetch-before/restore-after procedure).
namespace DiagnosticLog {

    // One log file's size: at this size it becomes the previous file (.old)
    // and a new one starts - so up to twice this on flash. Here, not in the
    // .cpp: ImageLibrary keeps room for the log to reach it (2026-11-07).
    constexpr size_t MAX_FILE_BYTES = 128 * 1024;

    // How much more flash the log may still take before it stops growing
    // (both files at MAX_FILE_BYTES, plus a batch of waiting lines).
    size_t growthLeft();

    // Call once after LittleFS is mounted (see Storage::begin()).
    void begin();

    // Appends one line: "uptime=<ms> clock=<local time|unsynced> [category] message"
    // Also prints the same line to Serial via CLOCK_LOG.
    void record(const char* category, const char* fmt, ...);

    // Something that is retried while it fails (a download, a lookup): one
    // line when it starts failing, one when it works again with the number
    // of failed attempts - not a line per retry, which would fill the log in
    // hours. One Streak per kind of failure, used from one task only.
    // `after`: log only from that many failures in a row (a single lost UDP
    // reply is normal, not news).
    struct Streak {
        uint32_t after  = 1;
        uint64_t failed = 0;   // 64-bit: a retry every 5 s fills 32 bits in 680 years
        void fail(const char* category, const char* what, const char* detail);
        void ok(const char* category, const char* what);
    };

    // Streams the current log file straight from LittleFS in small chunks -
    // deliberately NOT a String-returning readAll(): this log can reach tens
    // of KB, and building/copying that much into RAM for one HTTP response
    // crashed the device live (heap exhaustion) the one time it was tried.
    // The current file; with old=true the previous one (rotated .old) -
    // each streamed from flash on its own, so never both in RAM. The page
    // asks for both and shows them as one log (2026-11-07: the .old part
    // was never shown anywhere).
    void sendTo(AsyncWebServerRequest* req, bool old = false);

    // Lines wait and are written together while the screen stands still, by
    // loop() (DiagnosticLog.cpp, s_pending). A restart or crash keeps waiting
    // lines (begin() writes them).
    void loop();
    // Drops the waiting lines (factory reset: the new log starts empty).
    void discardPending();
    // Writes every waiting line now - before a restart the clock makes itself (Restart.h).
    void flushNow();

    // Batch writes to flash since boot, and how long they took - for
    // /api/status. Written from the main loop, the screen waits for them.
    struct WriteStats {
        uint32_t writes = 0, over50ms = 0, over100ms = 0, maxUs = 0;
    };
    WriteStats writeStats();

} // namespace DiagnosticLog
