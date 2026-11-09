# Limits: where each is defined, what depends on it, what a change would do

Started 2026-11-07, after the log grew from 2 x 32 KB to 2 x 128 KB and two
things broke quietly: copies of "32 KB" in the page, a test and a drill, and
a chain nobody had written down - the room left for pictures relied on the
log staying small, so pictures could have taken the room an update needs.

**Code wins** if this file disagrees with it.

## The rule

- **Defined once.** A limit a user can reach (a setting's range, an event's
  years, reminder days) is in `include/Config.h`, section LIMITS. A limit of
  one piece of code is at the top of that `.cpp` (TUNABLES), or in its header
  once a second file needs it (e.g. `DiagnosticLog::MAX_FILE_BYTES`).
- **The clock enforces it**, whoever sends the value: `Storage::clampConfig()`
  after every settings load (start-up, settings import) and change (the page,
  the API); events are clamped or refused when saved or imported.
- **The page and the tests ask the clock** - `GET /api/limits` - instead of
  keeping copies. Sliders and boxes carry `data-limit="key"` (and
  `data-limit-scale` when they count in other units); the page sets their
  min/max and its own checks from the reply. Tests take their expectations
  from the same reply (`sim/ui_tests.py` section `limits`).
- **Relations are checked by the build** where possible (`static_assert`):
  every default inside its range, the default window on the screen, the
  picture caps above the largest picture the page can make, the log's
  buffers in proportion. A change that breaks one doesn't compile.
- **What the page must keep itself** (its HTML has exactly three Wi-Fi slots;
  its calendar grid is built on the 12 Hebrew months) is checked against the
  clock's numbers by the tests, so a change on one side fails a test.

## The limits

| Limit | Defined | Used by / depends on it | Much larger | Much smaller |
|---|---|---|---|---|
| Screen 480 x 128 (`TFT_WIDTH/HEIGHT`) | Config.h | window ranges, picture height cap, icon width, picture caps (asserts) | page sliders follow (`/api/limits`); picture caps checked by asserts | same |
| Display window (`calibX/Y/W/H`) | Config.h LIMITS (`CALIB_*_RANGE`), defaults `DEFAULT_CALIB_*` | what is drawn; **pictures' height** (`pictureMaxH()` = window height, at most the screen) | - | pictures made smaller; at H 16 still drawable |
| Tear lines 0-15.5 ms (`CALIB_TEAR_US_RANGE`) | Config.h | Display.cpp waitForTe() | beyond one panel redraw (~15.5 ms) = the same as earlier in the next one | - |
| Brightness / room light 0-100 % | Config.h `PERCENT_RANGE` | BrightnessManager | - | - |
| Display time 2-60 s, scroll speed 1-10 px | Config.h | MessageAggregator, Display | slides wait long / scroll too fast to read | - |
| Font gaps / width scale | Config.h (no page control) | Font.cpp | letters drift apart, text leaves the window | letters overlap |
| Reminder days 0-30 (`EVENT_WARN_DAYS_RANGE`) | Config.h | **Calendar.cpp downloads holiday data for the longest reminder + 7 days**; MessageAggregator | measured on the simulator: 60 days -> 68 days of data, 22 KB cache; 365 -> 373 days, 123 KB cache - and on the clock ~18x the parse time and a 123 KB cache rewrite (a screen pause) | - |
| Event years 1800-2239 / 5560-5999 | Config.h (same span in both calendars - assert) | date conversion, the page's checks, Excel import | outside what the conversion was checked for | - |
| Debug time years 2000-2299 | Config.h `DEBUG_TIME_YEAR_RANGE` | `/api/debug/time` (moments via `daysFromCivil`, asserted) | dates the clock was never checked at (CLAUDE.md "Time is 64-bit") | - |
| Log file 128 KB, two files (`DiagnosticLog::MAX_FILE_BYTES`) | DiagnosticLog.h | **ImageLibrary's room for pictures reserves the log's full size** (`growthLeft()`); the page's "Debug always" warning; the log drill | less picture room (1 KB less for each KB); reading it takes longer on weak Wi-Fi (it is streamed, never in RAM) | less history |
| Log line 220 B, waiting room 3 KB / written at 2 KB | DiagnosticLog.cpp | RTC memory (lines kept over a restart) | RTC memory is small (8 KB on the S3, shared) | more flash writes (screen pauses) |
| Event picture file 64 KB (`IMAGE_MAX_BYTES`) | WebUI.cpp | upload check; assert: >= the largest still the page makes (screen height, 3:4) | - | refuses pictures (assert) |
| Your icons together 800 KB (`CUSTOM_IMAGES_MAX_BYTES`) | WebUI.cpp | upload check, Settings' storage bar | less room for event pictures | assert: >= one full-screen still |
| Room kept free on top: update + 160 KB + the log's growth | ImageLibrary.cpp | every picture upload, the storage bar | fewer pictures | an update may not fit (the reason for the reserve) |
| Animation frames <= 64, 20-2000 ms each | Display.h `PICTURE_MAX_FRAMES`, `PICTURE_FRAME_MS_RANGE` | reading pictures (Display, WebUI), the icon editor | more memory per animated icon (all frames in RAM) | - |
| Events 5000, recycle bin 200 | Config.h `MAX_EVENTS`, WebUI.cpp `MAX_DELETED_EVENTS` (assert: bin <= list) | parsing events.json; the real limit is free PSRAM (`EVENTS_PSRAM_*`) | PSRAM | - |
| Wi-Fi networks 3 | Config.h `WIFI_NETWORKS` | config.json keys, **the page's three slots (HTML)** - a test compares | the page needs more slots in its HTML | same |
| Event types and default icons | WebUI.cpp `EVENT_TYPES` | normalizing, Excel import, the page (`/api/limits eventTypes`) | - | - |
| When the timed slides show (From / Until per slide) | Config.h `ShowFrom`, `ShowUntil`, `SHOW_FROM_*` (the choices each slide offers), defaults `*_SHOW_DEFAULT` | `Storage::clampConfig()` (a choice a slide doesn't offer -> its default); the page's options (`CONTENT_EXTRAS`) - `sim/ui_tests.py` show_windows saves each one | - | - |
| Hebrew month names | JewishDateEngine.cpp | the page's calendar grid keeps its own list - a test compares | - | - |

## When adding or changing a limit

1. Define it once (see the rule); give it a unit in its name.
2. Clamp or refuse at the clock, wherever values come in.
3. If the page or a test needs it: add it to `/api/limits`, use `data-limit`
   or `LIMITS.*` - never a copy.
4. Ask what 10x and 1/10 would do: memory (is anything loaded whole?),
   flash space (and the reserves that count on it), time on a weak Wi-Fi,
   flash writes (screen pauses). Write the answer in the table above, and
   an assert or a test for any relation that must hold.
