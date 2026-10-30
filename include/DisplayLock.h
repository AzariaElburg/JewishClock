#pragma once

// Guards every access to DisplayManager's shared render state (_mode,
// _winX/Y/W/H, _currentElements, and the GFX canvas object itself) -
// written from the main task roughly every 16ms (DisplayManager::update())
// and from the AsyncTCP task (setWindow()/setMode()/
// getCurrentMessageElements(), called from WebUI.cpp's calibration and
// preview handlers). Same mutex pattern as EventsLock/DiagnosticLog.
//
// update() and a few others have early-return points partway through, so
// prefer Guard over manual take()/give() there - one missed give() on an
// early return is a deadlock the very next time either side tries to take
// the lock again. Internal helpers called only from within an already-
// guarded function (clearWindow(), resetScroll(), drawBmp(), etc.) must
// NOT take the lock themselves - this is a plain (non-recursive) mutex.
namespace DisplayLock {
    void begin();
    void take();
    void give();

    struct Guard {
        Guard()  { take(); }
        ~Guard() { give(); }
    };
}
