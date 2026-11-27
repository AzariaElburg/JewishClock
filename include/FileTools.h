#pragma once
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>

namespace FileTools {
    void skipUtf8ByteOrderMark(fs::File &f);
    void listDirRecursive(const char * path, const String& indent);

    // Whole files or none (2026-11-18): written to <path>.tmp and renamed over <path> only when
    // every byte is in it. A full flash takes part of a write and says so only in the count it
    // returns - the events save compared the file with that count, so a cut-off events.json passed,
    // replaced the good one, and every event was gone at the next start (found by the flash_full
    // drill). False = not saved, the old file untouched, the reason logged. Every file the clock
    // writes whole goes through these.
    bool writeJsonAtomic(const char* path, const JsonDocument& doc);
    bool writeTextAtomic(const char* path, const String& text);
}