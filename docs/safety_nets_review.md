# Safety nets: what the clock does when things go wrong (2026-11-18)

Code no test reaches (coverage run 3, `sim/coverage_report.py`), each either **tested** by forcing
the situation on the simulator, or **reviewed** in the code where the simulator can't produce it
(hardware). For each: the situation, what the code does, and the verdict. Line numbers move; the
function names don't.

## Tested (forced on the simulator)

| Situation | Where | Test |
|---|---|---|
| Flash full to the last block | every save | resilience `flash_full` - found a cut-off events.json passing as saved (all events lost at the next start); fixed: `FileTools::writeJsonAtomic/writeTextAtomic` |
| PSRAM almost gone | event saves, Excel import, pictures | resilience `low_memory` (`/psram-hog`) |
| Damaged pictures (every header check), garbage time file, BOM settings, impossible Hebrew date in the calendar file, no settings file, an old calendar file | Display BMP loader, NTP, Storage, Calendar | resilience `corrupt_files` |
| Hebcal answering a web page, "{}", a cut-off JSON | Calendar | resilience `service_garbage` |
| A time server lying, stalling, kissing, from another address | NTPManager | resilience `ntp` |
| Networks giving no address; the address taken back | WiFiManager | resilience `no_address` |
| An upload that stalls halfway; a package part of an unknown kind | Updater | resilience `update_bad_files` |
| Wrong input from any client | WebUI | web_ui `refusals` |

## Reviewed (hardware - the simulator has none of it)

| Situation | Code | What happens | Verdict |
|---|---|---|---|
| Flash won't mount | main.cpp setup(), `Storage::begin()` | LittleFS formats a flash it can't mount itself; failing even then = damaged flash: the setup waits without feeding the task watchdog -> restart every 20 s, mount tried again | right - nothing better is possible without a flash |
| Display panel doesn't start | `DisplayManager::begin()` | bus and panel freed, "SIMULATED", the clock runs on (web page works); `update()` tries again every 60 s (DISPLAY_RETRY_MS) | right |
| No memory for the 120 KB framebuffer | `DisplayManager::begin()` | **was:** `_canvas->begin()` unchecked - the first frame would write through a null framebuffer, at every start. **Now:** a failed start like the above (freed, logged, retried in 60 s) | fixed 2026-11-18 |
| The panel's TE line silent (wire, panel) | `waitForTe()`, `keepTeOn()` | no pulse for 4 periods: frames go at once, as without TE; the TE registers re-sent at most every 5 s | right (tearing setup untouched - hands off since 2026-11-09) |
| Light sensor missing at start | `AmbientLight::begin()` | logged, a fixed simulated reading; learning off without a sensor | right (the family's sensor is off by choice) |
| Light sensor failing after a good start | `AmbientLight::loop()` | the last good reading held. **Was:** silently, for ever. **Now:** one log line when reads start failing (3 in a row), one when they work again | fixed 2026-11-18 |
| mDNS doesn't start | `finishMdnsInit()` | logged; the health check rebuilds it later; the clock is reachable by IP and its own Wi-Fi either way | right (the family's router isolates devices - mDNS unused there) |
| A Wi-Fi scan can't start or gives nothing | `serviceRoaming()`, the page's network list, `servicePickScan()` | logged; roaming skips this round; the page gets an empty list; picking a network falls back to the saved order (not the one that just failed) | right |
| Wi-Fi driver states never seen (IDLE, SCAN_COMPLETED, NO_SHIELD...) | `statusName()` | names for the log only | right |

## Left as they are (safety nets that can't be reached and cost nothing)

- `switch` defaults over enums whose every value is handled - a trip-wire for a future value.
- Calendar: a second yom tov on a day, a third holiday on a day - no real date in 556 saved
  Hebcal downloads has them (checked 2026-11-17); the code keeps the first two and logs the rest.

## For the removal step (dead code, checked in the code - no caller)

`fontGetSpacing`, `fontGetWidthScale`, `fontGetGlyphLeftBearing`, `DisplayManager::getMode`,
`DiagnosticLog::flushNow`, `Timer::startMs`, `WebUI::stopServer`, `WiFiManager::stopAP`,
`Font.h isHebrew` (Display has its own), MessageAggregator's `defaultEventIcon` (every event gets
its icon when read - one table: `Storage::defaultIconForType`), Calendar's `"sunset"` category
branch (Hebcal never sends one in that answer), the upsert's "Hebrew date is missing its month"
warning (the input check refuses such an event first), `DisplayManager::setWindow`'s empty
calibration check, an empty debug branch in `drawGlyph`. Kept: `NTPManager::forceSync` (the
simulator's /ntp-sync uses it), the xRGB1555 picture format (a hand-made picture - tested),
the log level given as a number (a script's way).
