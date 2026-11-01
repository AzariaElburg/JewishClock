# JewishClock on the PC (simulator)

The clock's own code - calendar, slides, drawing, settings - compiled for the
PC, unchanged. Only the hardware is replaced: the screen becomes an image,
the file system a folder, Wi-Fi/NTP a simulated clock, downloads go through
`curl`. Because it is the same code, what it shows is what the clock shows;
`compare.py` / `moments.py` check that pixel for pixel against the real clock.

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
| Simulator vs the clock, 33 moments (holidays, Shabbat, fasts, sunset, rain/dew prayer switch) | `C:/pio3/jc_clean/venv/Scripts/python sim/moments.py --clock <clock-ip>` (~2 h; restarts the clock over USB, puts it back on real time at the end) |
| Rain/dew prayer slide (both switches), every day around each switch day, settings 0/1/2, evenings, nusach, abroad | `python sim/test_prayer.py` (~2 min) |
| Every slide of a whole year, to read through | `python sim/sweep.py --days 365` -> `C:/pio3/jc_clean/sim/work/sweep/slides.tsv` (~15 min) |
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
