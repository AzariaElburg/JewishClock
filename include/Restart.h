#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Every restart the clock makes on purpose goes through here (2026-11-21). Lines
// waiting for the log file (DiagnosticLog: written while the screen stands still)
// are written first: they would be kept in RTC memory, but the version that starts
// next may be an older one (a failed trial goes back) that doesn't read them - the
// night's numbers run lost the "failed its trial" line that way, the one line worth
// having after the fact. scripts/check_rules.py stops a build with ESP.restart() /
// esp_restart() / the rollback reboot anywhere else.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <esp_ota_ops.h>
#include "DiagnosticLog.h"

namespace Restart {

// Restart now.
inline void now() {
    DiagnosticLog::flushNow();
    ESP.restart();
}

// A firmware on trial that failed it: back to the previous version. Returns only
// if there is none.
inline void rollback() {
    DiagnosticLog::flushNow();
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

}  // namespace Restart
