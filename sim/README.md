# JewishClock on the PC (simulators)

Two programs, both the clock's own code compiled for the PC unchanged, only
the hardware replaced:

- **jcsim.exe - the screen simulator** (the rest of this file): calendar,
  slides, drawing, on SIMULATED time - any date, a day in seconds, repeatable.
  For "what does the screen show when".
- **jcclock.exe - the full simulator** (next section): the WHOLE program,
  `main.cpp` included, in real time - web pages, Wi-Fi, updates, backups,
  factory reset, restarts, its own NTP against real time servers. For
  everything the clock does besides drawing, and every test the clock itself
  was needed for (`utils/ap_tests.py --sim`).

## The full simulator (jcclock.exe, since 2026-11-05)

Every `src/*.cpp` unchanged on PC stand-ins for the hardware only
(`sim/clock_stubs`, then `sim/stubs`): web server, Wi-Fi driver, UDP and name
lookup, firmware slots, RTC memory, watchdog, PSRAM, light sensor. No PC
wall-clock time anywhere: after every start it counts from 1970 until the
clock's own `NTPManager.cpp` sets the time from a real server (the PC gives
only the ticks of `millis()`). Downloads (Hebcal, place) go out for real.

    python sim/build_clock.py        # after `pio run`: same version as firmware.bin
    C:/pio3/jc_clean/sim/build/jcclock.exe --fs C:/pio3/jc_clean/sim/clock/fs [--verbose]

- **Its files**: `--fs` = LittleFS (start from the staged image:
  copy `C:/pio3/jc_clean/fs/*` - page files gzipped, as on the clock); next to it
  `state/`: firmware slots (`flash/`), RTC memory, `world.json`.
- **Ports** (127.0.0.1): 8080 = the clock on the home network, 8081 = its own
  Wi-Fi (192.168.4.1 - the synced one, or the setup one), 8090 = control.
  A side that's down (not joined, restarting) doesn't answer, as on the clock.
- **The world** (`state/world.json`, made from the known networks at the first
  start): the networks and access points around - name, password, signal,
  channel, address, internet, on/off. `POST /world` on the control port
  changes it while running (a network switched off drops the connection,
  reason 200, as an access point vanishing does).
- **Control port** (tests, not the clock): `GET /state`, `POST /power-cut`
  (RTC memory lost), `POST /reset` (kept), `POST /stop`, `GET|POST /world`,
  `POST /light?lux=N` (`&sensor=0`: no sensor).
- **Updates across versions**: each build is also kept as
  `C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe`. A supervisor plays power
  supply and bootloader: it runs the slot's version's build, so an update is
  installed by the OLD version's code (as on the clock) and after its trial the
  NEW one runs; a version restarting while still on trial is rolled back. A
  package needs its version's build there (`build_clock.py` after `pio run`).
- **Memory**: internal RAM reports the clock's measured numbers; PSRAM is
  counted and refused at 90% of what the clock leaves free (fails first).
  Fragmentation is not modelled.
- **Checked against the real clock**: `sim/record_clock_web.py` recorded its
  answers (`reference/clock-<date>/web/`, out of git); `sim/compare_web.py`
  sends the same to the simulator - 69 of 69 the same on 2026-11-05 (status,
  header order, page files byte for byte, JSON keys and types).
  `utils/ap_tests.py --sim` runs all four trips against it (22/22, 2026-11-05).
- **Not on a PC**: the panel itself (speed, tear lines), flash-write pauses,
  real radio behaviour, heap fragmentation.

## The screen simulator (jcsim.exe)

The clock's calendar, slides and drawing on simulated time. The screen
becomes an image, the file system a folder, Wi-Fi/NTP a simulated clock,
downloads go through `curl` (cached). Because it is the same code, what it
shows is what the clock shows; `compare.py` / `moments.py` checked that pixel
for pixel against the real clock (`replay_moments.py`: against its saved
screenshots, without the clock).

## Setup (once)

1. Zig 0.13.0 (the C++ compiler; one zip, no installer) from
   https://ziglang.org/download/ - unpack to `%USERPROFILE%\.local\` or set `ZIG`.
2. `pio run` once (downloads ArduinoJson, which the simulator uses too).
3. `python sim/build.py` - builds `C:/pio3/jc_clean/sim/build/jcsim.exe` (~10 s).
4. `python sim/prepare_fs.py` - copies `Data/` plus the clock's live settings
   and events (`--local`: use `Data/`'s own; `--clock IP`: address if the name fails).

Rebuild (step 3) after changing the clock's code (`--exe PATH` builds a
second copy, e.g. while a long test is using `jcsim.exe`).

## Use

| What | Command |
|---|---|
| Watch it live in the browser, any date, any speed | `python sim/viewer.py` -> http://localhost:8765 |
| Slides of a moment as images | `C:/pio3/jc_clean/sim/build/jcsim.exe --fs C:/pio3/jc_clean/sim/work/fs --time "2027-04-21 15:00" --seconds 90 --out C:/pio3/jc_clean/sim/work/out` -> `NNN.bmp` + `slides.txt` |
| Simulator vs the clock, now | `python sim/compare.py --clock <clock-ip>` |
| Simulator vs the clock, 55 moments (holidays, Shabbat, fasts, sunset, years to 2300, rain/dew prayer around all three switches and their eves, settings 0/1/2) | `C:/pio3/jc_clean/venv/Scripts/python sim/moments.py --clock <clock-ip>` (~2.7 h; restarts the clock over USB, puts it back on real time at the end). A screenshot differing only in a few pixels in a flame (cake, candle) is the animation's phase, not a fault: the frame follows the uptime (`millis() / frame_delay_ms`, Display.cpp), which the simulator matches only roughly |
| The same moments without the clock: today's simulator against the clock's screenshots saved by the last `moments.py` run (2026-11-04/05, before the clock left) | `C:/pio3/jc_clean/venv/Scripts/python sim/replay_moments.py` (~5 min). Slides containing the characters of `--expect-changed` (default ׳ ״, raised after that run) may differ; to prove a change touched nothing else, build the code from before it (`build.py --exe`) and replay with `--exe PATH --expect-changed "#"` - must be all OK (done 2026-11-05 for the geresh: 103/103) |
| Rain/dew prayer slide (both switches), every day around each switch day, settings 0/1/2, evenings, nusach, abroad | `python sim/test_prayer.py` (~2 min) |
| Brightness learning on simulated days (widen, narrow after 3 days, gap, restart, millis wrap, saves) | `python sim/test_brightness.py` (seconds) |
| Every slide of a whole year, to read through | `python sim/sweep.py --days 365` -> `C:/pio3/jc_clean/sim/work/sweep/slides.tsv` (~15 min) |
| Does a wording stand still in alternating mode? Width in pixels of each line of a UTF-8 file, next to the window's (calibW) | `C:/pio3/jc_clean/sim/build/jcsim.exe --fs C:/pio3/jc_clean/sim/work/fs --instant-boot --measure FILE` |
| The Demo (Advanced settings' button) | `C:/pio3/jc_clean/sim/build/jcsim.exe --fs C:/pio3/jc_clean/sim/work/fs --out C:/pio3/jc_clean/sim/work/out --instant-boot --seconds 240 --demo-at 20` |

`--time` is Israel time (the clock's time zone from its settings); `--utc` or
`--epoch SECONDS` for UTC. It is the moment of **power on**.

## Startup and downloads take real-clock time

Like the clock, the simulator shows "Connecting to ..." until Wi-Fi is up
and its time is synced, then the date it computes itself, then the
downloaded calendar. Each step takes a random time within the range
measured on the real clock, widened for times not seen yet (table in
`sim_platform.cpp`; re-measure with `C:/pio3/jc_clean/venv/Scripts/python sim/measure_boot.py`,
which restarts the clock 8 times over USB). Every run prints its seed and
each value by name (`timing main#1 = 3912 ms`). `--seed N` repeats a run
exactly; `--timing main#1=6000,zmanim=7400` pins chosen values to recreate a
particular situation (names: `setup`, `wifi`, `ntp`, `geo#N`, `main#N`,
`zmanim#N`; without `#N`, every one of that kind). Each value comes from
(seed, name), not from a running sequence, so one more download somewhere
doesn't change the others. `--instant-boot` makes it all instant, for
checks of content only (`sweep.py`, `test_prayer.py`). A run stopped with
`--seconds` is a power cut: the next run on the same `--fs` starts from
whatever was saved, as the clock would.

## How it works

| File | Role |
|---|---|
| `sim_main.cpp` | Startup and main loop as in `src/main.cpp` (keep in step), 16 ms per frame, simulated time; writes images, `--match` compares with clock screenshots |
| `sim_platform.cpp` | Simulated `millis()`, the measured startup/download timing, file system on a folder, downloads via `curl` (cached in `C:/pio3/jc_clean/sim/work/http_cache`), the screen's frame buffer, the POSIX time-zone rules |
| `measure_boot.py` | Measures the real clock's startup and download times (source of the timing ranges) |
| `sim_modules.cpp` | Stand-ins for NTP, Wi-Fi, diagnostic log, locks |
| `stubs/` | PC versions of the Arduino/ESP32 headers the clock's code includes |

Downloads take simulated time, not real time, so a 90-second run still takes
about 2 s. Not simulated: brightness/night mode, the web pages, Wi-Fi drop-outs
after startup.
