"""
Builds the JewishClock simulator (C:/pio3/jc_clean/sim/build/jcsim.exe) with Zig's C++
compiler - a single portable download, no installation:
https://ziglang.org/download/ (tested with 0.13.0), unpacked anywhere;
point ZIG at zig.exe or put it on PATH.

    python sim/build.py

The clock's own source files (CLOCK_SOURCES) are compiled unchanged; only
sim/*.cpp and the headers in sim/stubs are simulator-specific.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_BUILD, LIBDEPS  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "sim"
OUT = SIM_BUILD
ARDUINOJSON = LIBDEPS / "esp32s3" / "ArduinoJson" / "src"

# The clock's code that decides and draws what is on screen - unchanged.
CLOCK_SOURCES = ["Calendar", "MessageAggregator", "JewishDateEngine", "Display", "Font",
                 "lang", "Storage", "FileTools", "LogLevel", "GeolocationService", "SunTimes"]
SIM_SOURCES = ["sim_main", "sim_platform", "sim_modules"]


def find_zig():
    for cand in (os.environ.get("ZIG"), shutil.which("zig"),
                 str(Path.home() / ".local" / "zig-windows-x86_64-0.13.0" / "zig.exe")):
        if cand and Path(cand).exists():
            return cand
    sys.exit("Zig not found: set ZIG to zig.exe (see the top of this file).")


def main():
    if not ARDUINOJSON.exists():
        sys.exit("ArduinoJson not found - run `pio run` once so PlatformIO downloads the libraries.")
    zig = find_zig()
    OUT.mkdir(parents=True, exist_ok=True)
    flags =["c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
             "-include", str(SIM / "stubs" / "sim_force.h"),
             "-I", str(SIM / "stubs"), "-I", str(SIM), "-I", str(ROOT / "include"), "-I", str(ARDUINOJSON),
             "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0",
             "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument"]
    sources = [str(SIM / (s + ".cpp")) for s in SIM_SOURCES] + [str(ROOT / "src" / (s + ".cpp")) for s in CLOCK_SOURCES]
    # --exe PATH: build somewhere else (e.g. while a test is using jcsim.exe).
    exe = Path(sys.argv[sys.argv.index("--exe") + 1]).resolve() if "--exe" in sys.argv else OUT / "jcsim.exe"
    cmd = [zig] + flags + sources + ["-o", str(exe)]
    print("building", exe, "...")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("BUILD FAILED")
    print("OK:", exe)


if __name__ == "__main__":
    main()
