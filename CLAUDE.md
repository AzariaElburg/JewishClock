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
The clock's own Wi-Fi password is a setting, made once per clock (the user,
2026-11-14): none in the settings yet -> AP_PASSWORD_DEFAULT if the build has one,
else random - written there, fixed from then on (WiFiManager::apPassword()). The
clean snapshot writes `AP_PASSWORD_DEFAULT ""`; this project still has the family's
(their printed start guide) only so their clock copies it into its settings with its
next update - **after that update, set it to "" here too: one firmware for every
clock**. Test: drill `ap_own_password`.

**Where constants go (decided 2026-10-16):** a hardware fact, or the default
of a setting saved in config.json, goes in `Config.h`. Anything else (retry
intervals, timeouts, thresholds) goes at the top of the one `.cpp` that uses
it, under `// === TUNABLES ===`, with its unit in its name, and moves to a
header only when a second file needs it. **Anything outside that file that
depends on the value (a test, the page, a script) asks the clock for it -
never its own copy, hard-coded or "soft-coded"** (the user, 2026-11-19): the file marks it
`TUNABLE_SHARED(NAME)` (include/Tunables.h) and the clock serves it at `GET /api/tunables`
(jcsim: `--tunables`; tests: `sim/clock_facts.py`). The screen's system messages are one list,
`include/Texts.h` (the screen code uses them by name), served at `GET /api/texts` (jcsim
`--texts`): a test asks for a situation's words (`clock_facts.screen_text()`), and a situation not
listed is a failed check saying so (the user, 2026-11-20 - "somewhere in the code" missed the right
words in the wrong place). Texts.h has three groups: SCREEN, ANSWER (web answers, `{}` filled by
`Texts::fill`) and LOG (printf formats: `DiagnosticLog::record` or `CLOCK_LOGF` - `CLOCK_LOG` takes
only a literal). Tests: `W().has(name, text, *fill)` (sim/clock_facts.py ClockWords). The page's own
messages: `PAGE_TEXTS` + `pt()` at the top of Data/index.html, tests `PW()` (web_ui.py, read from the
page). `scripts/check_rules.py` stops every build on a test literal copying either list (a deliberate
copy, e.g. a tool for an older clock: comment `copy-ok: <why>`); words NOT on a list are found by the
compliance run - `sim/scrambled_build.py` builds with every message letter-shifted (exceptions:
`sim/scramble_keep.txt`) and the full run must still pass. Its twin for numbers (2026-11-20):
`sim/nudged_build.py` - every TUNABLE_SHARED x5/4 or x4/5 (counts/days/hours +-1) and our own
LIMITS moved, by the run's seed (facts of hardware/standards never; rules and pairs in the script) -
the full run must pass there too. The pages never act on the clock's words: status codes and
headers (`apiSend()` in index.html; X-Update-Version / X-Update-Refused / X-Clock-Restarts). A value is described only where
it's defined; anywhere else, point to it. **To list every tunable: search
the code for `TUNABLES`.**

**Limits are defined once, across languages too (2026-11-07):** what a
setting or an event may be is in Config.h's LIMITS; the clock enforces it
(`Storage::clampConfig()`, events on save/import) and the page and the tests
ask `GET /api/limits` - never copy a clock number into index.html or a test
(`data-limit="key"` on a slider/box). Relations go in `static_assert`s. Every
limit, what depends on it and what 10x / 1/10 would do: **`docs/limits.md`**
- update it with any new limit. (A 32 KB copy in four places and a reserve
that silently assumed a small log were found when the log grew.)

**Saves: room first, events by Excel only (the user, 2026-11-18).** Every request that saves (settings,
places, events, keys...) asks `refusedForRoom()` (WebUI.cpp) first: no room -> 507 "Not saved: the
clock's storage is full", the page shows it - never "Saved" and lost later. A new saving route: the
same call. `/api/restore` takes settings only (events in it: refused) - events come in through the
Excel import alone (row checks, Undo, pictures). Test: resilience `flash_full`, web_ui `limits`.

**Texts taken in: one list (2026-11-13).** Every text the clock reads - a settings
change, an event, an import, a backup, its own config.json/events.json (an older
version's or edited by hand) - is checked by `Storage::configTextProblems()` /
`eventProblems()` against its limit (Config.h LIMITS, else its field). Typed or
imported: refused, every problem said. Already in the clock's files: kept as far as
the field holds and said in Settings (`/api/config` `textNotes`) until that text is changed - across
restarts too (`/text_notes.json`, 2026-11-15: the next save wrote the shortened text back and the
note was gone after a restart; a restore replaces them, factory reset removes them). A new text field
goes into that list; copies use `StringTools::utf8Copy` (whole letters), never
strlcpy/strncpy. Never assume a file holds only what our code wrote.

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
- **The page files go to the clock gzipped** (2026-11-04, `utils/web_files.py`):
  Data/ keeps them plain; uploadfs builds from `C:\pio3\jc_clean\fs` (staged by
  `scripts/stage_fs.py`), update packages take the same files. Page ready
  1.1 s -> 0.57 s, repeat visits 0.25 s (304). A new browser-only file:
  add it to `web_files.GZIP` and serve it with `sendWebFile()` (WebUI.cpp) -
  **not** the library's `send(LittleFS, path)`: its 304 queues the headers
  without sending them, they leave 40-580 ms later at lwIP's next flush.
- **Fastest route here is USB** (measured 2026-11-04): firmware writes in
  ~18 s (921600), uploadfs ~35 s in all. Wi-Fi (weak signal at the desk, -86 dBm)
  adds the 60 s trial and can't send data without firmware. `upload_speed`
  921600 (CP2102 on COM5; default 460800) halves the firmware write;
  uploadfs gains nothing - erasing/writing its 4 MB of flash takes ~18 s at
  any speed (only ~220 KB cross the cable, compressed).
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
  **Signed (2026-11-14):** make_update_package.py signs every package (ECDSA P-256,
  OpenSSL from Git for Windows) with the private key `%USERPROFILE%\.jewishclock\update_signing_key.pem` (never in git/OneDrive; `utils/update_key.py`); the firmware
  installs only packages its public key (`include/UpdateKey.h`) confirms - from a file
  or GitHub - and no plain .bin. The signature sits after JCEND!, so 2.1.2 (no check)
  still takes the first signed package. Firmware is made the one to start only after
  the check (UpdateSignature.h; the simulator checks with Windows' CNG).
  **From the internet (2026-11-14, OnlineUpdate.h):** the clock asks the latest release of
  the public repo `JewishClock-Updates` (Config.h UPDATE_FEED_REPO - named only there; the manual links it via /api/tunables) daily; a newer version
  (tag vX.Y.Z > its own) installs at 03:00 when "at night by themselves" is on, or with
  Install in Settings - the same Updater, signature and trial. The network part runs on
  Calendar.cpp's one download task (OnlineUpdate::runInTask()). Releases there get the
  .jcupdate as an asset (github_release.py - refused while the firmware still has a
  compiled-in AP password: the public feed must never carry the family's). Test: web_ui `online_update`
  (a feed stand-in on the PC; config `updateFeed`, not on the page).
  **Only a package completely there** (2026-11-15): the clock takes a release only if published (not
  draft/pre-release), its package GitHub-state "uploaded" with a size, and the download exactly that
  size; github_release.py makes the release a draft, uploads, reads the package back byte for byte,
  then publishes. No update key = nothing installed, like a wrong key (2026-11-17: Clear and
  "accepted without validation" removed; a missing/broken key is rewritten by the firmware at its
  start); a new signing key reaches a clock by Settings' "Paste a key", which replaces the old one
  (the key is on every feed release page; `update_key.py public`). GitHub's 404 for releases/latest = no release yet OR no
  repository: the clock then asks the repository itself and says which. The phone test with the
  family (the user, 2026-11-15): the one emailed version has 5-minute checks and install-at-once in its
  own commit (OnlineUpdate.cpp TUNABLES); the version after it, from the feed, sets them back - no
  switch kept in the code. Its log: Settings > Advanced > Log level Debug for a day.
  **The feed is monitored daily** (2026-11-15): `.github/workflows/update-feed-monitor.yml` runs
  `utils/check_feed.py` (repository there + public, latest release complete, package signed with the
  clocks' key); a failed run is emailed by GitHub. In this private repo only (not the feed's own -
  a deleted repo would take its monitor along; not in the clean copy). Scheduled workflows run from
  the default branch only: it starts when a release moves main. Test: `sim/test_check_feed.py`.
  **Hebcal is compared weekly too** (2026-11-18, `.github/workflows/hebcal-reference-monitor.yml`):
  `sim/make_hebcal_reference.py --check` (~70 s) asks Hebcal what `sim/hebcal_reference.json` holds
  and fails, emailed, if an answer changed - then judge it, refresh, rerun the own-calculations test.
  Both monitors write their problem as an error note (`::error::`), which GitHub's email shows.
  Problems the clock finds by itself ("Error - see Settings" on its screen) each get "OK, seen" in
  Settings; the line goes when every one is seen or put right.
  Tested 2026-10-17: good update, crash at startup, no Wi-Fi - all correct.
- Library versions in `pioarduino.ini` are exact on purpose; upgrade one
  deliberately and test it on the clock, never via a `^` range. After adding,
  removing or upgrading a library (or the framework, font, an online service):
  `python utils/make_licenses.py` (2026-10-31; see THIRD_PARTY_LICENSES/README.md).
- **Every release:** `python utils/make_source_bundle.py --kit` (2026-11-04) ->
  `C:\pio3\jc_clean\dist\sources\<version>\`: the exact source of every library and
  the Arduino core (`sources-*.zip`, ~60 MB - what the LGPL parts require) and
  a build kit (compiler, ESP-IDF libs for the S3, platform, Python wheels;
  ~635 MB in parts under GitHub's 2 GB per file) - attach them to the GitHub
  release, so the source can be handed over even if its authors drop it.
  Kit tested 2026-11-05 (empty folders, downloads blocked by a dead proxy,
  Python 3.12): builds. What it took: the platform fills its own Python
  environment (`penv`) online on every build and installs packages it
  doesn't even use (installer, gdb, PIO Home) - the kit carries all of
  `C:\pio3\packages`, pinned wheels of both environments, `kit_setup.py`,
  and builds with `PLATFORMIO_OFFLINE=1`. Steps: BUILD-KIT-<version>.txt.
- **Which version's data a version reads** (2026-11-15, `include/VersionRule.h` - one rule for
  updates, backup restores and Excel imports): its own major's (not newer) and the previous
  major's. Every Excel export names its version (a workbook defined name). Files without one
  (before 2026-11-15) are read while `ACCEPT_UNSTAMPED` - **at the second release after the first
  one that stamps, remove the flag and all code reading unstamped files** (the user, 2026-11-15;
  2026-11-19: removed, not set false - no dead switch kept).
- **Debug by itself around updates** (2026-11-15, the A -> B -> C phone test): LogLevel.h
  `Logging::Focus` + `Updater::debugFocus()` - the log at DEBUG while checking the feed,
  downloading, receiving a package and on trial; the setting untouched. **Remove after C
  installed itself from B** (the user: only for that test).
- **Every release gets its clean equivalent** (the user, 2026-11-09: part of a
  tagged release, always): `python utils/clean/snapshot.py --ref v<a.b.c>
  --commit MSGFILE --tag v<a.b.c>` - exactly the tag's files, scrubbed, committed
  and tagged in `JewishClock_clean`. A hit on a line read and judged harmless
  (a test's made-up network): `check.py --allow-line FILE N`.
- **GitHub** (2026-11-09): this project = private repo `JewishClock-Private`
  (remote origin), the clean copy = `JewishClock` (private for now). **Every
  commit is pushed right away** (`git push origin ai-assisted`); `main` = released
  versions only. **Last release step**, after the clean snapshot:
  `python utils/github_release.py <a.b.c>` - moves main to the tag, pushes both
  repos, makes both release pages (the family's package + PDFs here, sources +
  build kit on the clean copy); safe to repeat. Sign-in: Git Credential Manager,
  stored once. My shell sets GCM_INTERACTIVE=never and GIT_TERMINAL_PROMPT=0 - a
  new sign-in needs a separate window with both removed.
- **The manuals ship with the firmware** (2026-11-04): `Data/start.html`
  (one A4 page English + one Hebrew, Hebrew in Hebrew but the UI's English
  names) and `Data/manual.html` (English), linked in Settings > Clock
  software, in every update package. **Change a user-visible behavior or
  label -> update them in the same commit.** Each ends with "For clock
  software version ...", fixed in the file (2026-11-05, the user: the paper
  describes one firmware state, never changes by how it's viewed). **manual.html copies nothing from the
  clock** (the user, 2026-11-22; written 2026-11-26 in the skim style of the 2.1.2 printed manual; printed for a release by `utils/make_manual.py`): its pictures
  (inline, between `<!-- shot:NAME -->` marks - the clock serves no other files) are made by
  `python utils/manual_shots.py` from the working page on a simulated clock with made-up data (rerun after a
  page change; `--check` only checks); that run also checks every `<span class="ui">` name against the page's words,
  that the clock's screen texts shown in it (`data-text`, filled from `/api/texts` when viewed; the words inside
  are a fallback, compared) and numbers (`data-tunable` -> `/api/tunables`, the Wi-Fi address `data-apaddr`)
  exist, and that no "N days/minutes" is typed by hand. A new number or screen text in the manual: through
  those marks. Run it before stamping. After
  editing them, build, then `python utils/stamp_docs.py` (that build's
  version) and commit; for a release `stamp_docs.py 2.1.0` before the tag. The printed start guide gets the
  clock's Wi-Fi password only at print time (`start.html?ap=..&pw=..`, a
  local PDF) - never in the file, which goes to GitHub.
- **Before handing over a version:** `utils/ap_tests.py` (over the clock's own
  Wi-Fi: page, settings import, update install, then factory reset + restore
  from the exported Excel file on the setup page, compared with the start).
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
  v2.1.0 = the version handed over with the clock (2026-11-06; earlier
  v2.1.0/v2.1.1 tags were removed and their work folded into it - the user
  wanted the first release a round .0; the old tag objects are listed in
  `C:\pio3\jc_clean\dist\old_tags_2.1.x.txt`).
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

### Full simulator (`sim/`, jcclock.exe, 2026-11-05)
The clock left for the parents on 2026-11-06 - from then on every fix and
feature is tested on the full simulator: the clock's whole program (all of
`src/`, unchanged) on PC stand-ins for the hardware only, real NTP, real
downloads, web on 127.0.0.1:8080 (home) / 8081 (its own Wi-Fi), updates
across versions with trial and rollback. **Before every release, all of it in
one go: `python sim/run_all_tests.py --from <the family's version>`** - after the
builds, four groups (names: the user, 2026-11-11):
- **display** - what the screen shows, on jcsim's simulated time (prayer, brightness,
  show-windows, show-combos, retry, dates);
- **web-ui** (`sim/web_ui.py`) - every control of the pages, followed to the screen,
  and section `from_family`: a clock at their version with their data, updated over
  its own Wi-Fi to this build (everything kept), their old backup restored on a
  factory-new clock - this replaced the four trips in the run (2026-11-11);
- **resilience** (`sim/resilience.py`) - when things go wrong: Wi-Fi gone, power cuts,
  services down, bad updates, full flash, a complaint from a backup;
- **simulator-validity** (`sim/compare_web.py`): the simulator's web answers
  against the real clock's recorded ones. Only the stand-ins' part (library headers,
  status codes) still means much as the firmware drifts from the 2026-11-05
  recording - re-record at the parents' (`sim/record_clock_web.py`, or with only a
  browser: Settings > Clock software > Web answers > Record, emailed, then
  `sim/import_answers.py FILE` - bodies unpacked, headers as a set; 2026-11-14) and
  compare with that same version's simulator build. The list of requests is defined
  once, in Data/index.html (`RECORD_REQUESTS`); record_clock_web.py reads it there.
**Real-clock tools stay** for the laptop at the parents' (USB or the clock's Wi-Fi):
`utils/ap_tests.py` (the four trips), `sim/moments.py`, `sim/record_clock_web.py`,
`sim/measure_boot.py`. See `sim/README.md`.
**Fast, and any part alone** (the user, 2026-11-11: no step repeated where a shared
start can branch; nothing locked inside a big script). The four groups run side by
side, each with its own clocks and ports (jcsim at below-normal priority so the
real-time clocks keep their timing): ~1 h 45 min one by one -> see the summary's last
line. Resilience runs `--jobs` at a time (each its own process and ports, longest first
by the last run's durations.json: 57 -> 10 min at 6). `--only GROUP|STEP[:PART]` picks
anything: `display`, `resilience:light`, `web-ui:chains.24_hour`,
`web-ui:from_family.update`, `prayer:"abroad request"`, `show-combos:shabbat3`. Web-ui
parts: class `Parts` in web_ui.py - a part names what it `needs` (run alone, that
runs first); parts sharing a moment are a stage: one setup (the clock at a Friday),
then ONE watch of the screen for all of them (`expect` / `expect_absent` - "absent" =
two whole rounds, `/state` "rounds"). A new chain: a part in a stage, not its own wait. Since 2026-11-08 it builds the firmware and
its own update package (in its run folder, never dist - `--package` tests a
given one) and stages today's `Data/` itself: it used to take the newest
package in dist and the last USB upload's page files (both stale). Every
simulated clock has its own ports, in **`sim/sim_ports.py`** only - no test
runs on the work simulator's (a drill stopped it once; the trips factory-reset
their clock). Resilience tests that need a particular day move their own clock there
(`to_friday_morning()`): two failed just for running on a Sunday. **Flash a release from its tag, never from a
later commit** (the version would read "2.1.0-6-g..."): `git worktree add
C:/pio3/jc_clean/wt210 v2.1.0`, build and upload there with
`PLATFORMIO_BUILD_DIR=C:/pio3/jc_clean/pio/build210` (2026-11-06). **A stand-in must behave like the hardware/library
it replaces, never like a shortcut** (the user, 2026-11-05: no PC time, no
"replacing" project code). Real clock's answers and screenshots:
`reference/` (out of git, OneDrive-backed - an exception to "generated files
outside the project": they can't be made again without the clock).

**Two simulators side by side** (2026-11-07, `sim/start_clock.py family|dev`):
`family` = the parents' exact build (`sim/family_version.txt`, kept in
`C:/pio3/jc_clean/sim/versions/<v>/`, never rebuilt - `build_clock.py` won't overwrite
a release's build) with their synced data, on 127.0.0.1:8070 - reproduce their
complaints there, after loading their latest Excel export into it
(`sim/seed_from_export.py <file.xlsx>`: imported through their version's page,
read back, checked row by row; 2026-11-09); `dev` = the working build and `Data/`, on 8080 (each start =
a USB flash: its own copy of the build and the page files, its data kept).
Independent: an event made on one is not on the other. Each clock's window
title says which one it is and its version ("Released v2.1.1", "Work v...",
"Test (web-ui) v...": `JC_TITLE` from the starting script, set by the clock
itself on every start; builds before 2026-11-07 get it from start_clock.py).
The work clock is also named "JewishClockWork" (browser tab, page, start screen;
start_clock.py, 2026-11-12); the family clock keeps the family's name.
`--hotspot` makes a clock's own Wi-Fi a real one for phones (Windows Mobile Hotspot,
`sim/hotspot.py`, 2026-11-13; one clock at a time; see sim/README.md). Port 80 on this
laptop is held by IIS (W3SVC, never used) until `sim/hotspot_admin_setup.ps1 -StopIIS`.
start_clock.py opens each in a classic console window (`conhost.exe`), not a
Windows Terminal tab - tabs cut the title short; that one window outlives the
clock's restarts, and the supervisor's lines show there, not in run.log.

**A program the simulator starts (curl) goes through `simRun()`** (sim/sim_win.cpp, 2026-11-15),
never `system()`: that hands the child every file the clock has open, and while a download
ran the clock could neither rename nor remove its half-written picture - the "flaky"
pictures_fill failure, always at the calendar's second download. Proven with a small test
program before the fix; the flash stand-in's rename retries that had hidden it are gone.

**Every control of the web pages: `sim/web_ui.py`** (2026-11-07 as ui_tests.py, part of
run_all_tests; ~7 min at 3 lanes, its own clocks from 8060). Coverage is measured in the
page, not declared: every control ever on screen (each select option, both
states of a switch, rows and buttons made on the fly, delegated ones) must be
used by a test, else the run fails with "UNCOVERED". **A new control needs a
test there.** Results must be checked on the clock (settings, events, files,
`/state` "screen" = what it shows), not "the click worked". Found on its first
day: the setup page's network list stayed empty (asked once, the clock said
"scanning"), Detect hidden after Connect, dead headings looking clickable,
wrong-year dates passing.
**Follow the chain to the screen** (the user, 2026-11-10): "saved" is not the
end - whatever the family sees on the clock must be checked there too (section
`chains`: the event on its reminder day / day / Hebrew day, at debug time). Each
step passing alone hid two dead ends: an own emoji icon saved but blank on the
clock, and the 24-hour switch saved but used nowhere on it. A new setting or
event field: a chain to `/state` "screen", not just to `/api/config`. Brightness
goes to `/state` "backlight" (the PWM output, 0..1 - the ledc stand-in keeps its duty).
Also measure what a test only counts: the Hebcal resilience test counted log lines ("once,
not per try") for months while the clock retried every 5 s - seen only when its
fixed 11-min wait became "wait for 3 tries" (2026-11-11, Calendar.cpp retryWaitMs - since
then 10, 20, 40 s, then a minute, checked on simulated time by `sim/test_retry.py`).

**Screen checks ask the clock, they don't watch it** (the user, 2026-11-15: "why real time?"):
chains took 37 of a full run's 48 minutes watching slides go by. Now `Parts.settle()`,
`screen_shows()` and `never_rounds()` ask the running clock for its coming rounds -
`/state?rounds=N` -> `MessageAggregator::upcomingRounds()`, the screen's own round code
(`roundSlots()`, shared with the real rounds) with its live settings and events, so "saved but
not picked up live" still fails. Once a run, `preview_matches_screen()` checks in real time that
the announced round IS what the screen then shows. Full run since then: ~11 min (display in 3
streams, web-ui 4 lanes, online_update split in three); the floor is resilience's real 5-min
update trial. A new screen check: through these helpers, never a watch of real rounds.
Fixed pauses hidden in helpers count too (`data_button()`): the check after it waits.

**Waiting in tests** (2026-11-15): never a fixed pause before a check - `wait(cond)` until it
holds (a fixed 1 s before the event editor's date conversion failed under a full run's load);
"must not change" = `stays(cond, T)` (web_ui.py, resilience.py): watched the whole time, fails at
the first change. Short pauses between typing steps may stay. The less-used controls have chains
too (section `chains`, from "Item 9": Show year, every content row Off/back, Omer wordings, rain/dew
days + nusach, when Hebrew-date events start) - dates picked on jcsim first, so the expected text
is the clock's own.

**Test clocks never ask the real update feed** (2026-11-16): each clock asks 3 min after its
start, a full run starts dozens - eight runs used up GitHub's 60 an hour. `sim_ports.no_real_feed()`
on every test clock's flash; only web_ui's "real update feed" check asks GitHub.
**Every web_ui section starts on the clock as it was** (2026-11-16): the runner waits for today's
downloaded calendar (`calendar_settled()`: Daf Yomi in `/api/calendar`), measures settings and
events before and after each section and puts back what it left (`put_back()`; "NOT PUT BACK" in
the lane's out.txt otherwise). Before, calendar_content left every row at "every round" and wifi
the home network's place in the home city - the next section in the lane ran on that.
**The page: answers can come back in any order** (2026-11-16, found under a full run's load):
a request per keystroke applies only the latest one's answer (handleAutoConvert); live saves go
one at a time (flushLiveSave) - two at once reached the clock in either order, an older slider
value was saved last. New on-typing requests: the same.

**Time-dependent checks run on simulated time** (the user, 2026-11-08): what the screen
shows at a moment (candles, day change, fasts, ...) is checked on jcsim - two minutes of
slides in ~3 s (`sim/test_show_windows.py`: 51 moments in ~45 s) - not by waiting on the
full simulator, which runs in real time (real NTP, real web). web_ui checks the page's
controls and what the clock saved; the screen at other times belongs in a jcsim test.

### PC simulator (`sim/`, 2026-10-17)
The clock's calendar/slide/drawing code compiled for the PC unchanged
(Zig as compiler), hardware replaced by stand-ins - see `sim/README.md`.
Use it to check what the screen shows at any date without touching the
clock; `sim/moments.py` checks it against the real clock pixel for pixel.
When `src/main.cpp`'s setup/loop changes, update `sim/sim_main.cpp` to
match; a new `src/` file the display path needs goes into `sim/build.py`.

**Checking the code (2026-11-14, nothing to install):** `utils/code_check.py`
(clang-tidy + the Clang Static Analyzer, from VS Code's C/C++ extension),
`utils/find_copies.py` (copy-paste finder), ruff/pylint in `C:\pio3\jc_clean\venv`,
and `run_all_tests.py --uptime-start half|wrap|1000y` (every simulated clock
starts as if up 24.8 days, 49.7 days or 1000 years - what a fresh clock never
shows; all three before a release). None of them caught a `strlcpy` with a wrong
size - copies go through `StringTools::utf8Copy` so that mistake can't be written.
`scripts/check_rules.py` stops every build (firmware and simulators) on a rule
the compiler can't check - today: no `millis()`/`micros()` outside Uptime.h.

**Every fix: the same mistake everywhere, through one helper** (the user,
2026-11-14). A bug fixed at one spot is not done: find every place with the same
pattern, fix them all through one shared function, list the places in the commit.

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
`/log/diagnostic_log.txt` on LittleFS (own folder: in the root it made LittleFS compact the root's change list every ~9 lines, ~250 ms), capped at 128 KB per file with two-file rotation (32 KB until 2026-11-07; the page, Get log and the sync script read both files)
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

### The clock runs for years: uptime is 64-bit (2026-11-14)
Nothing may go wrong because the clock has been up N days, for any N up to 1000
years. `millis()` is 32-bit (wraps every 49.7 days; signed comparisons go wrong
from 24.8) - it stopped NTP for weeks (a "0 = now" read as a moment 24.8+ days
ago). All of the firmware's timing is `Uptime::ms()`/`us()` (64-bit esp_timer,
292,000 years) and `Timer` (include/Uptime.h: "not running" is its own state,
never a special time value); `scripts/check_rules.py` stops a build with
`millis()`/`micros()` anywhere else. Counters that grow by themselves: 64-bit
unless they provably last 1000 years at their real rate. NTP's 32-bit seconds
are read past 2162 by the latest time seen (`/time_seen.txt`, NTPManager.cpp
timestampUnixMs(): the era nearest it, RFC 5905's +-68 years - since 2026-11-17; the
"first at or after seen - 1 year" rule turned a real 2026 answer into 2162 after a wrong
saved 2036). Libraries (Wi-Fi, web server, lwIP) still use 32-bit millis() -
the simulator replaces them, so `--uptime-start` doesn't test them.

### System messages are English; the status slide (2026-11-14)
Messages about the clock itself (all in `include/Texts.h`) - "Connecting to ...", "Waiting for the time...",
"Internet unreachable", "Clock's Wi-Fi: <name> - password <pw>", "No NTP for 3 days" - are
English in every display language (the user); events, holidays, dates follow the
language. The status slide (MessageAggregator.cpp offlineNotice()): the clock's own Wi-Fi
(its name now: the clock's or "...-Setup") every round for 3 minutes after a start and
whenever offline - no lockout, the clock has no buttons; online but no time server for 3
days: the longest NTP wording that fits the window (fit by measuring). Test:
`sim/test_status_slide.py` (jcsim `--ntp-silent-hours`).

### Dates: DateRules.h is the one place (2026-11-14)
Every date that comes in - typed, imported, restored, the clock's own files,
Hebcal's answers, the calendar cache, `/api/calendar/convert`, debug time - is
checked by `include/DateRules.h` (30 Feb never; 29 Feb / 30 Cheshvan / 30 Kislev /
30 Adar I / Adar II only without a year or in a year that has it), and event
dates in other years are mapped there (29 Feb -> 1 Mar) or by JewishDateEngine's
Hebcal rules. A new way in for a date calls DateRules; never a bare sscanf. The
page asks the clock before an Excel import (`/api/events/check`) and names each
problem's row and the file's own column headings. The mapping functions check their
own input (resolveGregorian / resolveBirthdayOrAnniversary / resolveYahrzeit: a date that
isn't one maps to nothing) and give only real dates.

### Time sync follows the SNTP rules (2026-11-17, NTPManager.cpp)
Until then a reply was taken if its time field wasn't zero, and the gradual correction ran the
wrong way (a clock 1 s behind ended 2 s behind - proven with the code copied into a test). Now
(RFC 4330/5905/8633): a random stamp per question that the reply must carry back (no late,
repeated or forged reply), sender = the address asked, mode 4, LI != 3, stratum 1-15,
kiss-o'-death obeyed, root distance < 1 s; half the round trip added. Off < 50 ms: left;
< 1 s: slewed at 5 ms/s (never backwards); more: a jump only when a second reply (the next
server's) agrees within 0.5 s - also a first sync > 400 days from `/time_seen.txt` (which then
follows a confirmed earlier time). A first opinion on a jump counts 4 x SAME_SERVER_MIN_MS
(CONFIRM_MAX_AGE_MS); two that disagree = a failed ask, again in 64 s doubling - not the regular
interval (2026-11-21, found by a numbers run under load). The regular question: every 5.5-6 h,
drawn anew after each sync (SYNC_JITTER_MS - the NTP Pool: "at random times").
**Politeness, from the sources, not guesses** (2026-11-21, the user: "read the reply and act upon it,
always, everywhere"): never the same server within SAME_SERVER_MIN_MS = 60 s (RFC 4330 s5; s10's 15 s
is the bare floor; Cloudflare/Google state none) - a forced sync waits too. A kiss-o'-death RATE = the
longer of 5 min (doubling while repeated) and the kiss's own poll field, at most 2 h (RFC 5905 s7.4,
RFC 8633, the reference implementation); DENY/RSTR = never again until it leaves the list or a restart
(duration: an open question to the user); others = an unusable reply. A server's name looked up again
at most hourly after trouble, else daily (NTP Pool), each server's address kept by its slot.
**A silent server: the internet checked first, at once** (the user's design, 2026-11-21): nothing else
asked meanwhile; `include/NetHealth.h` checkNow() - one small request to CONNECTIVITY_CHECK_URL, or any
server's answer since the silence. Internet there -> that server's fault; none -> nobody's, watched
(a check at most every PROBE_MIN_GAP_MS), and when it answers the time is asked at once "as if nothing
happened". Same for a name without an address. **"Not recommended"** (troubleWith()): a server that
failed while the internet worked (silent, unusable reply, kiss, no address) rests (60 s doubling to
SILENT_REST_MAX_MS while failing), goes to the bottom - asked only while every recommended one rests
after a failure (one merely asked within the minute is waited for) - and the next is asked at once.
Once a day (20-28 h, random) it gets one question; TROUBLE_GOOD_DAYS good answers on separate days clear
it, a failure starts the count again. Kept by name in `/ntp_trouble.json` (isClockDataPath, written on a
still screen), `/api/status` "ntpTrouble", shown under its box in Settings > Advanced.
**Test clocks never ask the real servers** (that night's "outages" were the test runs themselves: dozens
of clocks from the home address got rate-limited by a pool server - "RATE" kisses, silent drops):
`sim/time_relay.py` answers them with the PC's time (JC_NTP_RELAY, started by run_all_tests /
web_ui / resilience; sim_net.cpp hands the answer over as from the server asked). Real servers only
for drill boot_ntp (`Clock(real_ntp=True)`), the old-version drills and from_family, and the tests'
reference `clock_facts.ntp_now(SAME_SERVER_MIN_MS)` - spaced in real time across all test processes
(`C:/pio3/jc_clean/ntp_reference_rest.json`), kisses obeyed. (A days-long watch of the three servers,
utils/ntp_watch.py, found the cause within minutes - a "RATE" kiss while a test run was on - and was
removed the same day; its replies: reference/ntp_samples_2026-11-21.txt.)
**Any server's "wait"** (HTTP 429/503 Retry-After; GitHub's 429 / 403 rate limit and its reset) is
obeyed: Calendar's one download helper, OnlineUpdate's feedGet(). Drills: resilience `ntp`
(sim/resilience.py FakeTimeServers: 127.0.0.1-3 and `fresh()` ones - each part on servers never asked,
so the 60 s rule doesn't stretch the drill; port 123 redirected by the simulator's /ntp-port; /ntp-sync
= a question as soon as one may be asked; /state "utcMs") and `ntp_outages` (a silent server with the
internet up, the internet cut mid-question and back between two checks).

### Midnight: one moment per main-loop pass (2026-11-21)
The user: midnight "should either be ignored or taken into account - but not just left to chance".
`main.cpp` `loop()` calls `NTPManager::beginPass()` first and `endPass()` last: every time read in
between (getTime / getDateTime / getLocalEpoch, any module, the web task too) is that one moment - a
new day only comes between passes, never between the calendar's step and the slides'. Every place
that uses the date or time, and its verdict: `docs/midnight.md`. Test: `sim/test_midnight.py`
(display group). A new main-loop step that reads the time needs nothing more; one that runs in its
own task reads the live time - say there why that is safe.

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
2026-10-30 as unneeded. A restart the clock makes itself writes them first:
every one goes through `include/Restart.h` (2026-11-21 - a failed trial's own
line was lost: the older version it went back to didn't read RTC memory);
check_rules.py stops a build with a direct `ESP.restart()`. Any new automatic write should use the same check.
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
`CONFIG_ASYNC_TCP_RUNNING_CORE=0` (pioarduino.ini): on core 1, at priority 10
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
  letters (text cell FONT_H 72 of the 85-row window; since 2026-11-04 the
  cell is 80 rows, letters 48 tall, window 97). Tuned by eye,
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

### Picture sizes come from the display window, never a fixed number (2026-11-07)
Event pictures and replaced icons are made in the page as tall as the clock
shows a picture: the window height `calibH` (at most the screen's 128 rows;
Display.cpp `pictureMaxH()`), event pictures 3:4 wide (`setPictureSize()` in
index.html). Until then a fixed 72x96 from a prototype's narrower window -
pictures made then stay as they are (tiles take each picture's own size).
Only the Hebrew-date mark keeps its own small height (`BI_OWN_HEIGHT`).

### A reset doesn't power-cycle anything wired to the board
The BH1750 (or any peripheral) draws power from the ESP32 board's own 3.3V
regulator output, not an independent supply. A reset (button/RTS pulse)
only restarts the CPU via EN — the regulator keeps running, so the
peripheral never loses power, and a stuck I2C bus state won't clear.
**Fully unplugging the ESP32's power does fix this** (it cuts the
regulator itself), since the peripheral has nowhere else to draw from.

### Backlight: a flicker can be a half-on MOSFET module (2026-11-04)
The backlight flickered about once a second, worse over time, better after
tapping the case. Not loose wires: the LR7843 module switched only half on
(at 100%: 1.2 V from LED-K to GND instead of ~0, 45 mA instead of ~140), so
small wobbles of the 5 V/3.3 V came through. Proof: a jumper from its Load to
GND -> brighter, steady. Replaced by a "15A 400W" dual-MOSFET trigger module
(VIN+/VIN-/OUT+/OUT-, TRIG/PWM/GND): steady. Wiring: Config.h (backlight) and
the wiring document. Check: LED-K (FPC 34) to GND at 100% below 0.05 V.

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
The day's Hebrew date is category `hebdate` (its date in the field `hdate`, "20 Tevet 5787") -
until 2026-11-17 the parser looked for category "hdate" and never read it. Hebcal's numbers win
(JewishDateEngine::parseHebcalDate), our wording stays; a disagreement is logged. Our own date
and sunset/tzeit calculations are checked against real Hebcal answers by
`sim/test_own_calculations_vs_hebcal.py` (21 Hebrew years day by day, 8 extreme places, 400 yahrzeit /
Hebrew-birthday dates from Hebcal's yahrzeit API for 30 Cheshvan, 30 Kislev, Adar I/II; the reference made
once by `sim/make_hebcal_reference.py` - Hebcal's zmanim answer at most 180 days at a time).

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
Saves are now temp-file-then-rename (`FileTools::writeJsonAtomic()` / `writeTextAtomic()` - every
file the clock writes whole; since 2026-11-18 checked against the length it SHOULD have: on a full
flash the old check passed a cut-off events.json, and every event was gone at the next start).
Always restore the real time (offset 0) at the end of a test.
The clock answers 409 until its first time sync after a restart (an offset
taken from its unsynced 1970 time overflowed to 1948 - 2026-10-18). Scripted
version of all this, restart via a USB reset pulse included: `sim/moments.py`.
