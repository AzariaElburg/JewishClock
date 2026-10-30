#pragma once

// Guards WiFiManager's state-machine globals (g_state, g_stateTimer,
// g_retryCount, g_lastConnectAttemptMs, g_restartAt, g_apActive, the
// g_disconnectEventTriggered/g_gotIpEventTriggered/g_lastDisconnectReason
// flags, and the diagnostic counters) - written from three different
// contexts: the main task (WiFiManager::loop()), the WiFi event task
// (WiFiManager::WiFiEventHandler, via WiFi.onEvent), and the AsyncTCP task
// (the AP-portal's POST /save handler, which calls setState() directly).
// Same mutex pattern as EventsLock/DisplayLock/NTPLock.
//
// setState() itself does NOT take this lock - it's called both from
// already-guarded entry points (loop(), the /save handler) and would
// deadlock taking it a second time on a non-recursive mutex. Guard at the
// outermost entry point only (loop(), WiFiEventHandler(), or explicitly
// around a direct setState() call), same pattern NTPManager uses for its
// own call-each-other public functions.
namespace WiFiLock {
    void begin();
    void take();
    void give();

    // True if the calling task currently holds this mutex. setState() uses
    // this to catch a future call site that reaches it without going through
    // a Guard first - that's exactly how the race this lock fixes got in in
    // the first place, so leaving setState() able to silently run unlocked
    // again would be a footgun. Logs loudly rather than asserting/crashing:
    // this ships on a device nobody is going to debug live, so a loud,
    // persisted warning that doesn't take the clock down beats a crash.
    bool heldByCurrentTask();

    struct Guard {
        Guard()  { take(); }
        ~Guard() { give(); }
    };
}
