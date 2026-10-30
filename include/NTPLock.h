#pragma once

// Guards every one of NTPManager's module-level time-model variables
// (g_haveSync, g_baseMillis, g_pendingSlewMs, etc.) - read/written from
// the main task (loop(), and indirectly via Calendar.cpp/MessageAggregator.cpp
// calling getTime()/getDateTime()), the AsyncTCP task (WebUI.cpp, notably
// POST /api/location calling begin() - a full reset, mid-flight, of
// everything loop() is also touching every tick), and even the WiFi event
// task (DiagnosticLog::record() calls isSynced()/getDateTime() from
// wherever it's invoked). Same mutex pattern as EventsLock/DisplayLock.
//
// Take this once per PUBLIC NTPManager:: function, for its whole body -
// several of those functions call each other (getDateTime() calls
// getTime(), begin() calls setTimezone()), so only the outermost call
// may take the lock; the ...Internal() helpers in NTPManager.cpp exist
// so that inner call doesn't try to take it again and deadlock.
namespace NTPLock {
    void begin();
    void take();
    void give();

    struct Guard {
        Guard()  { take(); }
        ~Guard() { give(); }
    };
}
