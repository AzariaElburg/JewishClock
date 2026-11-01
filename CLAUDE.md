# JewishClock — notes for editors (human or AI)

**`include/Config.h` is the single source of truth for all pin assignments.**
The full physical wiring (connector by connector, backlight MOSFET circuit,
sensor module pins) is `docs/hardware/ESP32_S3_Clock_Master_Wiring_Matrix_Pinout.docx`.

**Folders (2026-10-17): `Data/` = everything that goes onto the clock** (web
page, images, the Excel tools the clock serves for download). Everything
else stays on the PC: `utils/` (PC-side scripts, e.g. image generators),
`assets/images/` (source art), `docs/hardware/` (wiring, datasheet). Never
keep a second copy of a `Data/` file elsewhere - the copies drift apart.

**Generated files live outside the project folder (2026-10-30):** the folder
is backed up by OneDrive, so builds, the simulator's test files (1+ GB),
update packages and the Python tools (pyserial etc.) go to **`C:\pio3\jc_clean\`**
- `utils/local_dirs.py` (and `pioarduino.ini` `build_dir`/`libdeps_dir`).
All of it can be rebuilt; never point a build back into the project folder.

**Clean copy (2026-10-30):** this project is the complete private one and is
never scrubbed. `JewishClock_clean` (next to it) gets snapshots without
private details: `python utils/clean/snapshot.py [--commit MSGFILE]` copies the
committed files, swaps settings/events for blank samples and real network
names/passwords for placeholders, then refuses if `utils/clean/check.py`
finds anything (rules + fingerprints of the live private values; hooks there
check every commit too). `utils/clean/` exists only here. A new kind of private
detail the check misses: add a rule or `check.py --add`, not a one-off edit.

**Where constants go (decided 2026-10-16):** a hardware fact, or the default
of a setting saved in config.json, goes in `Config.h`. Anything else (retry
intervals, timeouts, thresholds) goes at the top of the one `.cpp` that uses
it, under `// === TUNABLES ===`, with its unit in its name, and moves to a
header only when a second file needs it. A value is described only where
it's defined; anywhere else, point to it. **To list every tunable: search
the code for `TUNABLES`.**

## Project Overview

One-off gift, not a published product: a single clock for the author's
parents, replacing one over 35 years old (also a gift from the author).
The old clock: analog face, wooden case, with a slit below housing a
monochrome LCD matrix display, 4 buttons, and a microcontroller — hi-tech
for its era — showing Hebrew dates, holidays, personal events, candle
lighting. **Trigger for this project**: the old microcontroller's built-in
date tables ran out at the end of 2025. This rebuild uses an ESP32-S3 +
NV3041A display + BH1750 light sensor + PWM backlight, pulling live data
(Hebcal, NTP, IP geolocation) instead of baked-in tables — so it doesn't
expire again.

Scope grew well past a like-for-like replacement ("if we're doing A, why
not B/C/X/Y/Z") — the Web UI, auto-dimming, image/animation support, and
most networking-reliability work exist because of that, not the original
clock's requirements.

**Keep this file current.** Code wins if this disagrees with it. First
*edit* an entry to stay aligned with reality; only *prune* it once it's no
longer relevant at all. Add an entry when something would've saved real
time or prevented a mistake, and isn't obvious from the code itself. Keep
entries short and dated, pointing at real evidence (file/line) over
re-telling the story.

## Build & Flash

- `pio run -c pioarduino.ini -t upload` — firmware (`src/`, `include/`).
- `pio run -c pioarduino.ini -t uploadfs` — filesystem (`Data/`), as a **separate, full-replace**
  image. This includes `Data/config.json`/`events.json` — if those local
  files are stale, this **overwrites the device's live settings and
  personal events**. Flashing one target never updates the other. This also
  wipes the log (`/log/`, see below) — **mandatory** sequence, every
  time, no exceptions (scripted: `python utils/sync_from_device.py pre`,
  uploadfs, `python utils/sync_from_device.py post`):
  1. `POST /api/debug/diagnostic-log/note` body `msg=Data flashing initiated`
  2. `GET /api/debug/diagnostic-log` → save as `Data/log/diagnostic_log.txt` (rides
     along in the new image instead of being lost)
  2b. `GET /api/backup` → write its `config` and `events` into
     `Data/config.json` / `Data/events.json`, so settings and events changed
     through the UI survive the upload. Abort if it doesn't parse or looks
     wrong (e.g. no `wifiSSID`); never upload stale local copies over live data.
     Write them compact, one line, like the committed files - pretty-printed
     JSON turns a value-only sync into a whole-file diff.
  2c. Event pictures (2026-10-21): empty `Data/images/`, then for each entry
     of `GET /api/images/list` save `GET /api/images/get?id=<id>` as
     `Data/images/<id>.bmp`. Gitignored, never in update packages; their
     backup is the Excel export (index.html). See `include/ImageLibrary.h`.
     Same for the clock's own pictures the family replaced: empty
     `Data/custom/`, then each `custom` entry of `GET /api/builtin/list`
     -> `GET /api/builtin/get?which=custom&name=<name>` as `Data/custom/<name>.bmp`.
  3. `pio run -c pioarduino.ini -t uploadfs`
  4. Wait for the device to reboot and reconnect
  5. `POST /api/debug/diagnostic-log/note` body `msg=Data flash complete`
- **Over Wi-Fi - how the parents' clock gets updates (emailed to them):**
  `pio run -c pioarduino.ini` (check [SUCCESS]), then `python utils/make_update_package.py` ->
  `C:\pio3\jc_clean\dist\JewishClock-update-<version>.jcupdate`: firmware + all `Data/` web
  files, never config/events/log. Installed from the settings page:
  Firmware Update -> choose file -> Update (`curl -F "firmware=@<file>"
  http://<clock>/update` does the same). New firmware runs **on trial**: it
  must join Wi-Fi within 5 min and stay up 60 s, else the clock returns to
  the previous firmware by itself; the package's web files are installed
  only after the trial passes (and discarded on rollback), so firmware and
  page always match. The clock stores only the web files that changed, and
  on install removes every file the version doesn't list, except its own
  data (`isClockDataPath()` in Config.h - add any new file the firmware
  writes there, or the next update deletes it). Design and format: `include/Updater.h`. If the web page
  itself is ever broken: `http://<clock>/update-safe` (built into firmware).
  Tested 2026-10-17: good update, crash at startup, no Wi-Fi - all correct.
- Library versions in `pioarduino.ini` are exact on purpose; upgrade one
  deliberately and test it on the clock, never via a `^` range. After adding,
  removing or upgrading a library (or the framework, font, an online service):
  `python utils/make_licenses.py` (2026-10-31; see THIRD_PARTY_LICENSES/README.md).
- **Firmware 2.x builds with a separate PlatformIO** (2026-10-18). 2.0 moved
  to Arduino-ESP32 3.x (ESP-IDF 5.5, 64-bit time - see the gotcha below),
  which only the community **pioarduino** platform supports. Its platform
  installs its own fork of PlatformIO's core into whatever PlatformIO runs it -
  run by the official PlatformIO, that half-failed and broke it (removed
  packages; repaired). So it lives on its own, touching nothing else:
  Python venv `%USERPROFILE%\.pioarduino\venv` (pip package `pioarduino`),
  packages/toolchains in **`C:\pio3`** (short on purpose: the framework has
  ~200-character paths and long paths are off on this PC). Build:
  `$env:PLATFORMIO_CORE_DIR="C:\pio3"; & "$env:USERPROFILE\.pioarduino\venv\Scripts\pio.exe" run -c pioarduino.ini`
  (add `-t upload` to flash). The official PlatformIO -
  `"$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"` - still builds 1.x
  (tag `pre-64bit`) unchanged. PlatformIO isn't on PATH here either way.
  **The project file is `pioarduino.ini`, not `platformio.ini`** (2026-10-25):
  VS Code's PlatformIO extension (the OFFICIAL PlatformIO) treats a folder
  with a `platformio.ini` as its project, and on edits of it installed the
  3.x framework into its own packages (2026-10-22, and 2026-10-25 despite
  its settings; both times deleted again - 1.x refetches its 2.x). Without
  that file it leaves this folder alone. Every pio command here takes
  `-c pioarduino.ini`. Tag `pre-64bit` (1.x) keeps its own `platformio.ini`.
- **Versions come from git** (2026-10-26, `scripts/gen_version.py` writes
  `src/Version.cpp`, not in git): a release = a tag `v<a.b.c>`
  (`git tag v2.0.1`); a build of exactly that tag with the firmware sources
  committed is called just "2.0.1" - what the family sees. Anything else is
  e.g. "2.0.1-3-g1a2b3c4" (3 commits later, at that commit), "-dirty" with
  uncommitted firmware changes (Data/config.json/events.json don't count).
  Tags: v1.2.0 (= `pre-64bit`), v2.0.0 (start of 2.0, 2026-10-18).
  Major = new foundation / one-way step (2.0: framework 3.x), minor =
  features, patch = fixes. The old `version_base.txt` + `build_number.txt`
  (up to 2.0.0.0101) are gone; their compiler define made every build a full
  rebuild of everything (5-9 min).
- `Data/config.json`: commit it only when its *structure* changes (new or
  removed keys), together with the code that caused it. Value-only changes
  from syncing with the clock stay uncommitted - settings legitimately vary
  over time.

### COM5 / serial (2026-10-15/16)
- Upload failing with "Could not open COM5, the port doesn't exist" usually
  means another process holds the port, not that it's missing - typically a
  `pio device monitor` (e.g. VS Code's Serial Monitor) left open. Find it
  with `Get-CimInstance Win32_Process` (look for `device monitor`).
- Opening the port (any serial tool) can pulse RTS/EN and restart the board.
- `pio device monitor` refuses to run without an interactive terminal; from a
  script use pyserial (`C:\pio3\jc_clean\venv\Scripts\python`), with a fixed read duration so
  it closes the port itself.

### PC simulator (`sim/`, 2026-10-17)
The clock's calendar/slide/drawing code compiled for the PC unchanged
(Zig as compiler), hardware replaced by stand-ins - see `sim/README.md`.
Use it to check what the screen shows at any date without touching the
clock; `sim/moments.py` checks it against the real clock pixel for pixel.
When `src/main.cpp`'s setup/loop changes, update `sim/sim_main.cpp` to
match; a new `src/` file the display path needs goes into `sim/build.py`.

## Gotchas

### Headers belong in `include/`, not `src/`
A header placed in `src/` next to its `.cpp` silently shadows the real one
in `include/` (quoted `#include` resolves same-directory first). Happened
to `BrightnessManager.h` — fixed 2026-10-07; the shadowed copy in
`include/` had also drifted out of sync (wrong `begin()` return type).

### LittleFS paths are case-sensitive
`/img/` ≠ `/IMG/`; mismatches fail silently (missing file, no error).
Currently clean — no case-mismatched duplicates in the repo (checked
2026-10-07) — but re-check if a load ever silently comes up empty.

### Wi-Fi: don't redo first-boot work on every reconnect
This device reconnects often under marginal signal (brief blips, not just
outages). The code used to treat every reconnect like a first boot —
rebuilding the HTTP server, cycling mDNS inline, re-syncing geolocation,
rebuilding the AP — which under poor signal snowballed into a fully
unresponsive device. Fixed in `WebUI.cpp`/`WiFiManager.cpp`; read those
comments before changing reconnect behavior again. Also added: a 20s task
watchdog in `main.cpp` (the ESP-IDF default only covers CPU0's idle task,
not Arduino's `loop()`).

### Wi-Fi: which access point the clock is on (2026-10-17)
The home network has several access points with the same name (log of a
roam scan: `AP-1` ch9 ~-71 = the usual one, `AP-2` ch9 ~-96 = far node).
Up to three networks are known (2026-10-21: `wifiSSID`, `wifiSSID2`,
`wifiSSID3`). `connectSTA()` joins the last connected one first
(`wifiLastNetwork`, no scan); only if that fails a quick scan picks the
strongest other one (`servicePickScan()`, logged `joining "..." (heard ...)`).
Settings' "Connect" button moves it on purpose (`Connect pressed for ...`).
Connecting always joins the strongest (`connectSTA()`, full-channel scan);
while connected, `serviceRoaming()` moves to a clearly stronger one if the
signal stays weak - tested by walking away with a phone hotspot: switched
in 7 s. **Gotcha:** a phone hotspot with the same name+password counts as
part of the network - the clock (and the PC) happily join it and get a
different subnet (a different subnet). If the clock "stops answering" at its usual
IP, try `jewishclock.local` and check `/api/status` "ip" before theorizing.
The `WIFI reconnected` log line names the access point (`ap=`) and channel.

### Never run a blocking call (HTTP, or anything else) from the main loop task
`Calendar.cpp`/`GeolocationService.cpp` used to run synchronous HTTP fetches
inline in `Calendar::update()`, on the same task as Display rendering and
the web server. One fetch measured freezing the whole device for 2.75s —
long enough to kill an in-progress page load. Not a WiFi quality issue:
external-server round-trips are just inherently slower than anything local,
and blocked everything else regardless of signal. Fixed by moving only the
network I/O to a dedicated FreeRTOS task (`Calendar.cpp`'s "Async HTTP GET
helper"); parsing/caching stays on the main thread. This is the *only*
background task in the codebase — before adding another, measure that the
synchronous version actually blocks something, the same way this one was
found, using `main.cpp`'s `TIMED_STEP` instrumentation (kept in permanently
now — it was removed once before, only for this exact bug to resurface).
Since 2026-10-24 that task also parses the main calendar download and writes
the calendar cache file (from a snapshot copy), once per refresh - measured
on the display loop before: 43 ms parse, ~155 ms per save (twice).
NTP's server-name lookup (2026-10-16) is non-blocking too, but without a
task of our own: it's handed to lwIP's existing task (`tcpip_callback` →
`dns_gethostbyname_addrtype`) and the answer arrives by callback - see
`NTPManager.cpp`. Prefer that pattern where a library offers a callback.

### Persistent event history: `DiagnosticLog`, not another one-off file
For anything worth correlating after the fact (reconnects, retries, boot
events) across hours/days, not just the live serial view: `DiagnosticLog::record(category, fmt, ...)`
(`DiagnosticLog.h`/`.cpp`). Always prints live *and* appends to
`/log/diagnostic_log.txt` on LittleFS (own folder: in the root it made LittleFS compact the root's change list every ~9 lines, ~250 ms), capped at 32KB with two-file rotation
(`.old` on overflow) — survives reboots, not `uploadfs` (see above). Fetch
via `GET /api/debug/diagnostic-log`, viewable in the UI's Advanced section.
Added 2026-10-08 after an mDNS investigation where only live serial logs
were available and the issue had already passed by the time anyone looked.
Each flash write is a short screen pause (measured 2026-10-25: up to 287 ms
per line with open/close), so lines wait and go in one write when the
screen stands still (see "Writes the clock does by itself", below). They wait in RTC memory
(kept by a restart, crash or watchdog reset - not by a power cut, nor by a
USB reset pulse, which counts as power-on). Reading the log writes them
first. `/api/status` `logWrites` = writes and their times.
`LOG_AT(...)` persists too (it calls `record()`). Anything retried while it
fails (downloads, lookups) uses `DiagnosticLog::Streak` instead (2026-10-24):
one line when it starts failing, one when it works again - a line per retry
filled the log. The log starts right after the filesystem mounts (main.cpp),
so failures loading settings/events are in it.

**Log timestamps must never take a lock** (2026-10-16). `record()` used to
get its time from `NTPManager::getDateTime()`, which takes NTPLock - and
NTPManager logs while already holding NTPLock, so a logged DNS failure
deadlocked the main loop and the watchdog rebooted the clock every ~25 s.
Timestamps now come from `debugAdjustedNow()` (`include/DebugTime.h`):
lock-free, debug offset included, "unsynced" until NTP's first sync this
boot. Anything that logs must not call back into a module that may be
holding a lock while it logs.

**Don't `req->send(200, contentType, someString)` for arbitrary/dynamic
content.** That overload round-trips through `const char*`
(`WebRequest.cpp`'s `send()` calls `content.c_str()`), and
`AsyncBasicResponse` then assigns that into its own `String` via
`operator=(const char*)` — which stops at the first NUL byte, silently
truncating the response to whatever came before it, with Content-Length
*matching* the truncation (so no size mismatch to notice). Hit live
2026-10-11: one stray NUL landed in the diagnostic log's very first-ever
line, and `GET /api/debug/diagnostic-log` returned the same truncated ~16
bytes forever after — looking exactly like the log had stopped growing,
even though `record()`/`readAll()` were both working fine the whole time
and the file was genuinely tens of KB and growing. Cost hours chasing
filesystem corruption and task-concurrency theories before checking the
response path itself.

First fix attempt — `req->beginResponseStream(contentType)` +
`response->write(data, len)` — solved the NUL-truncation (explicit length,
no `const char*` round-trip) but turned out to still build the entire
content in RAM before handing it to the response buffer, i.e. two full
copies alive at once. The first time the log was actually large (tens of
KB), that double-buffering exhausted the heap and crashed the live device.
**Real fix**: stream straight from LittleFS instead — `req->send(LittleFS,
path, contentType)` (`DiagnosticLog::sendTo()`) if the content already is a
file, or write it to a temp file first and then do the same
(`req->beginResponse(LittleFS, tmpPath, contentType)`, see `/api/backup`)
if it's computed in memory. `AsyncFileResponse` reads the file in small
chunks on demand — genuinely one copy in RAM at a time, confirmed via the
library source, not just by analogy. Reserve `beginResponseStream` for
content you're certain stays small (it never got fixed at a general level,
just avoided here) — anything that *could* grow large needs the file-based
pattern, not this one.

### Every JSON/text response sends `Connection: close`
`WebUI::sendJson()`'s `req->send(200, contentType, String)` builds an
`AsyncBasicResponse`, which unconditionally adds `Connection: close`
(library default) — the client can't reuse the TCP connection for its next
request, no matter what it asks for. Harmless for one-off requests; real
cost appears if something ever fires several requests back-to-back, since
each pays a fresh connect, and the browser's per-origin connection limit
(6 in Chrome) queues the rest behind that. Confirmed live 2026-10-11: the
Web UI's `fetchHebrewOnlyAges()` fired one `/api/calendar/convert` POST per
Hebrew-only event (~26 on this family's event list) — a HAR capture showed
~2.5s of page-load stall, almost entirely this. Fixed there by computing
the values server-side in `/api/events/list` instead of removing the N+1
round trips one-by-one (see that handler). Not fixed at the response-header
level — only matters again if some future feature reintroduces a request
burst; if so, this is the first thing to check.

### ESPAsyncWebServer: a registered route also matches every path under it
`g_server->on("/api/events", ...)` matches not just that exact URL but also
`/api/events/anything` — `AsyncCallbackWebHandler::canHandle()` (library
source, `WebHandlers.cpp`) treats a plain URI as a prefix via
`url.startsWith(uri + "/")`, and the server dispatches to the *first*
registered handler whose `canHandle()` returns true (method must also
match — a GET handler never swallows a POST request to the same path).
Registering `/api/events/delete` and `/api/events/import` *after*
`/api/events` (POST) meant every request to those two was silently
swallowed by the shorter route's handler instead — both returned its
"Missing body" error, never their own code. Found live via `curl -v`,
confirmed by hitting an unrelated nonsense sub-path (`/api/events/blahblah`)
and getting the same wrong response instead of a 404.

Fixed properly 2026-10-10 by never leaving a handler on the bare `/api/events`
path at all — every endpoint under it now has its own named sub-path
(`/api/events/list`, `/api/events/upsert`, `/api/events/delete`,
`/api/events/import`, `/api/events/excel-template`). With no handler sitting
on the parent path itself, nothing registered under it can ever collide,
for *any* method, regardless of registration order. Prefer this over
registration-order juggling for any future endpoint family under `/api/`:
give the "plain" operation (list/get) its own sub-path too, don't leave it
on the bare parent.

### The place follows the Wi-Fi network; time zones come from Data/tz.csv (2026-10-24)
Each known network has its own place (`Location.h`); joining a network makes
its place active - no monthly re-lookup (it only re-guessed from the IP). The
clock's time runs on a POSIX rule; the services give only zone names, so
`Data/tz.csv` maps name -> rule. Regenerate with `node utils/make_tz_table.js`
when the world's time-zone rules change (it checks every rule hour by hour for
10 years and leaves out zones without a fixed rule; Israel uses its built-in
rule). Local time is `localtime_r()` - never `t - mktime(gmtime(t))`, which is
an hour off for hours around each summer-time switch (was in
`getLocalEpoch()` until 2026-10-24; caught by testing four cities at their
switch moments with the debug time).

### Time is 64-bit since 2.0 - keep it that way (2026-10-18)
Arduino-ESP32 2.x had 32-bit `time_t`: wrong dates from 19 Jan 2038 (seen
for real when a too-early test date wrapped the clock to 1948). 2.0 uses core
3.x (64-bit `time_t`); `NTPManager.cpp` has a `static_assert` that fails any
build with 32-bit time. Never store seconds-since-1970 in `long`/`uint32_t`
(32-bit on the ESP32 - and on Windows, where the simulator had its own 2038
bug); use `time_t`/`int64_t`. NTP's own 32-bit seconds wrap on 7 Feb 2036 -
`recvPacket()` picks the era (valid to 2162). Checked on the clock at 2038,
2040, 2100, 2240, 2300 (`sim/moments.py`, y2038... moments).

### Core 3.x: a Wi-Fi event handler must never take a lock (2026-10-18)
In core 3.x, `WiFi.mode()` waits until the Wi-Fi start event has been through
every registered handler. `WiFiManager` calls it while holding `WiFiLock`, and
its event handler took the same lock - the first connect deadlocked (both CPUs
idle, 20 s watchdog reboot). The handler now only writes atomics; keep it so.
Also new in 3.x: `File::name()` is only the file's own name - use `path()`.

### Writing flash pauses everything - including the screen (2026-10-18)
While the ESP32 erases/writes flash, both cores pause any code running from
flash for the duration (tens to hundreds of ms per erase). During an update
upload (2 MB, ~25 s) every display frame was cut into pieces - 250-750 ms
per frame in the log ("SLOW STEP: DisplayManager::update"), a visibly
flickering screen. Now a still "Updating..." frame is shown for the whole
upload (`DisplayManager::showStill`, driven from main.cpp via
`Updater::uploadInProgress()`). Same cause, smaller: loading an image from
flash the first time it was shown (1.1 s for the candles) - images are now
preloaded at startup. A "SLOW STEP" on the display step during a flash
write is the pause, not the drawing; check what was writing at that moment
before suspecting the display code. Also: the main loop touching LittleFS at
all (even `exists()`) waits while another task writes a file (~230 ms
"SLOW STEP: MessageAggregator::loop" during log writes from the web task,
gone with the change) - checks for
pictures use `DisplayManager::hasImage()` (memory) for that reason, and the
calendar cache is written only when its content changed (2026-10-25).
**Writes the clock does by itself wait for a still screen** (2026-10-30):
the log, the calendar cache, learned light and a network's place / last
network (`Storage::saveConfigSoon()`) are written when
`DisplayManager::goodMomentToWrite()` says so - the screen has stood still
for 0.3 s, or after 60 s of waiting; in single-scroll mode (never still)
after 2 s, so a burst goes as one write. Writes the user asked for
(settings, events, pictures) go at once. Failure lines wait like the rest
on purpose: waiting lines survive a crash or watchdog reset (RTC memory),
only a power cut loses them - an "urgent" path was built and reverted
2026-10-30 as unneeded. Any new automatic write should use the same check.
`/api/status` `screenStill` shows it live; checked 2026-10-30: 18 of 18
log writes on a still screen.
**Measure hitches, don't guess** (2026-10-22): `/api/status` has
`displayHitches` / `displayBigHitches` (frame gaps > 100 / 250 ms since boot);
read them before and after repeating one kind of request. That's how the
picture list (opening 28 files, 0.9 s) was found to freeze the screen while
picture downloads didn't. Lists and downloads of pictures now come from the
display's memory (`DisplayManager::imagesIn()` / `imageFile()` - the same
bytes as the files); uploads still hitch (flash writes, unavoidable).

### The web server runs on core 0, alongside the main loop (2026-10-22)
`CONFIG_ASYNC_TCP_RUNNING_CORE=0` (platformio.ini): on core 1, at priority 10
over `loop()`'s 1, every slow request froze the screen for its whole length.
Now handlers run truly in parallel with the main loop. A handler may read
main-loop data, but must change it only under its lock (EventsLock,
DisplayLock, NTPLock, WiFiLock) or by setting a request the main loop
carries out (atomic flag - e.g. `Calendar::clearCacheAndRefresh()` /
`requestManualGeoSync()`, done in `Calendar::update()` step 1a). Never load
a file while holding DisplayLock: the screen waits for the whole read
(`DisplayManager::isImageUsable()` reads first, then locks to store).

### Scroll tearing (2026-10-25/26) - settled; read before touching the display link
A tear line moved through scrolling text: the panel redraws itself from its
own memory every ~15.5 ms on its own clock, and a frame sent regardless gets
caught half-sent. **Settled setup (2026-10-26):**
- SPI at **80 MHz** (Config.h TFT_SPI_HZ; the chip datasheet 8.3.2 allows a
  12.5 ns write cycle, the module sheet says 50 ns - 80 runs clean here).
- Only the X/Y/W/H **window** is sent (Display.cpp sendFrame(); the whole
  panel only while calibrating): ~15 ms per frame at 80 MHz (22.5 at 40,
  whole panel 35.8 at 40 / 23.4 at 80).
- Each frame starts **12 ms** after the panel's TE pulse (waitForTe()). The
  two tear lines then stand still in the empty bands above and below the
  letters (text cell FONT_H 72 of the 85-row window). Tuned by eye,
  12 +-0.5 ms. Changing window position/height, speed or font can move them
  back into the text - retune with Calibration's **"Tear lines"** slider
  (saved: config `calibTearUs`, default Config.h DEFAULT_CALIB_TEAR_US;
  2026-10-27, for the new box with a larger display frame).
- **Pictures** at full window height (PICTURE_MAX_H_ROWS): up to 68 rows they
  stay clear of the lines, but look too small; the user chose 85 - their top
  and bottom edge can tear. A slow scroll (speed 2, what the parents will
  use) makes the remaining tear much less noticeable than speed 7-9.

**Hardware facts:**
- **TE wire:** FPC pin 8 (header 33) -> GPIO 12 (Config.h TFT_TE, wiring doc
  conn. 24). The pad only drives once vendor register TE_CTRL (E7h) = 0x10
  (te_out_oe, between FF=A5 / FF=00) plus the standard 0x35. Sent right after
  the panel's init it didn't take; keepTeOn() sends it between frames
  whenever TE is silent for 1 s. Pull-down on the pin: floating, it gave
  ~110 fake pulses/s. Registers are only in the chip datasheet
  `docs/hardware/NV3041A_chip_datasheet_V1.2` (pdf + text), not the module's.
- The panel scans **~272 lines per redraw** (TE mode 1: 544 pulses per
  15.5 ms), our 128 rows among them - every timing model built on "128 rows
  + blanking" was wrong. Measure, don't model.
- **QSPI** (4 data lines, ~5-6x faster) is possible, not done: D0 = SDA (FPC
  13, GPIO 10), D1 = DCX (FPC 11, GPIO 11), D2 = DB2 (FPC 19 / header 22),
  D3 = DB3 (FPC 20 / header 21); straps SPI4W/IM1/IM0 = 0/1/1 (FPC 40/39/38).
  Chip datasheet 6.3, Table 6-1-1; library: Arduino_ESP32QSPI.
- A loose Dupont wire looked exactly like an 80 MHz failure (black screen,
  backlight on, TE silent or irregular) - check wiring first.

**Tried and dropped** (git 115378b..a95a2a6): sending only changed rows
(parts of letters blinked under TE timing); the DMA bus (Arduino_ESP32SPIDMA
left the screen blank as dropped in, not investigated); the Advanced "live
SPI speed" switch (never took effect - removed).

### A reset doesn't power-cycle anything wired to the board
The BH1750 (or any peripheral) draws power from the ESP32 board's own 3.3V
regulator output, not an independent supply. A reset (button/RTS pulse)
only restarts the CPU via EN — the regulator keeps running, so the
peripheral never loses power, and a stuck I2C bus state won't clear.
**Fully unplugging the ESP32's power does fix this** (it cuts the
regulator itself), since the peripheral has nowhere else to draw from.

### Arduino confirms new firmware by itself unless told not to
ESP-IDF's rollback only protects you if the app confirms itself *late*. The
Arduino core calls `esp_ota_mark_app_valid_cancel_rollback()` in
`initArduino()` - before `setup()` - unless the weak hook
`verifyRollbackLater()` returns true (`esp32-hal-misc.c`). Until 2026-10-17
nobody overrode it, so the clock had no rollback protection at all (an older
comment in main.cpp assumed it did). Overridden in `Updater.cpp`; don't
remove it.

### Never filter routes with the library's `ON_STA_FILTER`
It accepts only requests whose destination IP is the STA address, so every
request arriving through the clock's own AP (192.168.4.1) was rejected,
fell to `onNotFound`, got redirected to `/`, was rejected again - an endless
redirect loop (ERR_TOO_MANY_REDIRECTS). Found 2026-10-15. Use
`ON_STA_OR_SYNCED_AP_FILTER` (`WebUI.cpp`) for every new route.

### Hebcal: holiday kind is in `subcat`, not `category`
For holidays `category` is always `"holiday"`; `"fast"`, `"major"`,
`"shabbat"` etc. live in `subcat`. Comparing `category` to `"fast"` silently
never matched (2026-10-15). Hebcal also sends its own fast times as
`category:"zmanim"` items - match them by `title_orig` ("Fast begins" /
"Fast ends"), which stays English regardless of `lg=`. Tisha B'Av's begin
item sits on the eve; Yom Kippur has none (candles/havdalah cover it).

### Rain/dew slide outside Israel has a known gap (2026-11-01)
Built for Israel; abroad works except that Ashkenazim abroad get no reminder
to stop משיב הרוח at Pesach (they say nothing - the slide doesn't mention what
isn't said). Decisions and points to check: `docs/rain_dew_abroad.md`.

### Testing with the debug time offset (`POST /api/debug/time`)
The offset is saved in `config.json` and applied at boot. Since 2026-10-21 the
route also takes `{"offset": seconds}` or `{"local": "YYYY-MM-DDTHH:MM"}`
(Advanced > Debug time: shift / set to a Gregorian or Hebrew date) and makes
the clock fetch the new date's calendar right away - no restart needed for
that any more. If you do restart, first let saves finish and confirm
`config.json` is intact (`GET /api/backup`): on 2026-10-16 a test restart
was followed by an EMPTY config.json and the clock fell back to setup mode.
Saves are now temp-file-then-rename (`writeJsonAtomic()` in `Storage.cpp`).
Always restore the real time (offset 0) at the end of a test.
The clock answers 409 until its first time sync after a restart (an offset
taken from its unsynced 1970 time overflowed to 1948 - 2026-10-18). Scripted
version of all this, restart via a USB reset pulse included: `sim/moments.py`.
