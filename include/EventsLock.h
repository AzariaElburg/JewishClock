#pragma once

// Guards every access (read or write) to the shared EventList (g_events) -
// mutated from the AsyncTCP task (WebUI.cpp's delete/import/upsert
// handlers) while the main task iterates it every ~second (Calendar.cpp,
// MessageAggregator.cpp). Same mutex pattern as DiagnosticLog's own.
//
// Call take() immediately before touching the vector and give()
// immediately after - keep the critical section to just the vector
// access itself (plus Storage::saveEvents(), which is local LittleFS I/O,
// not network), never anything that can block for a while.
namespace EventsLock {
    void begin();
    void take();
    void give();

    // RAII alternative to a manual take()/give() pair - use this instead
    // wherever the guarded code can throw (e.g. PsramAllocator's
    // std::bad_alloc on a full/fragmented PSRAM heap), so an exception
    // unwinding through still releases the lock instead of leaving it held
    // forever and deadlocking every future access from any task.
    struct Guard {
        Guard()  { take(); }
        ~Guard() { give(); }
    };
}
