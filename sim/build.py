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
CODE = ROOT   # the clock's src/ and include/ (--tree: another copy)
ARDUINOJSON = LIBDEPS / "esp32s3" / "ArduinoJson" / "src"

# The clock's code that decides and draws what is on screen - unchanged.
CLOCK_SOURCES = ["Calendar", "MessageAggregator", "JewishDateEngine", "Display", "Font",
                 "lang", "Storage", "FileTools", "LogLevel", "GeolocationService", "SunTimes", "ImageLibrary", "Tunables", "NetHealth"]
SIM_SOURCES = ["sim_main", "sim_platform", "sim_modules", "sim_win", "sim_coverage"]


def find_zig():
    for cand in (os.environ.get("ZIG"), shutil.which("zig"),
                 str(Path.home() / ".local" / "zig-windows-x86_64-0.13.0" / "zig.exe")):
        if cand and Path(cand).exists():
            return cand
    sys.exit("Zig not found: set ZIG to zig.exe (see the top of this file).")


# --coverage (2026-11-16): the same program with line counting - llvm-mingw's clang (Zig lacks the
# counting runtime), -fprofile-instr-generate; sim_coverage.cpp writes the counts. A measuring
# build only: run_all_tests.py --coverage, then a normal build again.
LLVM_MINGW = Path(os.environ.get("LLVM_MINGW", "C:/pio3/llvm-mingw/llvm-mingw-20261104-ucrt-x86_64"))


def compiler(flags):
    """[compiler] + flags - Zig normally; with --coverage, llvm-mingw's clang with counting."""
    if "--coverage" not in sys.argv:
        return [find_zig()] + flags
    clang = LLVM_MINGW / "bin" / "x86_64-w64-mingw32-clang++.exe"
    if not clang.exists():
        sys.exit("llvm-mingw not found at %s (set LLVM_MINGW)" % LLVM_MINGW)
    rest = [f for i, f in enumerate(flags) if f != "c++" and f != "-target" and (i == 0 or flags[i - 1] != "-target")]
    return [str(clang)] + rest + ["-fprofile-instr-generate", "-fcoverage-mapping", "-DJC_COVERAGE=1", "-static"]


def link(cmd, exe):
    """Compiles/links `cmd` (everything but "-o") into `exe` - through a new file swapped in, so a
    program file held for a moment (2026-11-20: "Permission denied" writing jcsim.exe right after a
    PC restart - most likely the virus scanner on the fresh file) is waited for, up to 30 s. Every
    simulator and test program is linked through here. False: the build failed."""
    import subprocess as sp
    import time
    new = exe.with_name(exe.stem + ".new" + exe.suffix)
    if sp.run(cmd + ["-o", str(new)]).returncode != 0:
        return False
    for _ in range(30):
        try:
            os.replace(new, exe)
            return True
        except PermissionError:
            time.sleep(1)
    print("%s held by another program for 30 s (one still running?)" % exe)
    return False


def main():
    # --tree DIR: the clock's code from another copy (sim/scrambled_build.py) - the stand-ins stay these
    global CODE
    if "--tree" in sys.argv:
        CODE = Path(sys.argv[sys.argv.index("--tree") + 1]).resolve()
    if not ARDUINOJSON.exists():
        sys.exit("ArduinoJson not found - run `pio run` once so PlatformIO downloads the libraries.")
    if subprocess.call([sys.executable, str(ROOT / "scripts" / "check_rules.py")]):
        sys.exit("build stopped: scripts/check_rules.py")   # as the firmware build
    OUT.mkdir(parents=True, exist_ok=True)
    flags =["c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
             "-include", str(SIM / "stubs" / "sim_force.h"),
             "-I", str(SIM / "stubs"), "-I", str(SIM), "-I", str(CODE / "include"), "-I", str(ARDUINOJSON),
             "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0",
             "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument"]
    sources = [str(SIM / (s + ".cpp")) for s in SIM_SOURCES] + [str(CODE / "src" / (s + ".cpp")) for s in CLOCK_SOURCES]
    # --exe PATH: build somewhere else (e.g. while a test is using jcsim.exe).
    exe = Path(sys.argv[sys.argv.index("--exe") + 1]).resolve() if "--exe" in sys.argv else OUT / "jcsim.exe"
    print("building", exe, "...")
    if not link(compiler(flags) + sources, exe):
        sys.exit("BUILD FAILED")
    print("OK:", exe)


if __name__ == "__main__":
    main()
