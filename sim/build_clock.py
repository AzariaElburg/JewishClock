"""
Builds the FULL simulator, jcclock.exe: every src/*.cpp of the clock unchanged
(main.cpp's setup()/loop() included, NTPManager.cpp asking real time servers),
on PC stand-ins for the hardware only (sim/clock_stubs, then the screen
simulator's sim/stubs). Same compiler as sim/build.py (Zig).

    python sim/build_clock.py

-> C:/pio3/jc_clean/sim/build/jcclock.exe, and a copy as
   C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe - the build an update to
   that version runs (sim/sim_clock_main.cpp). The version is src/Version.cpp's,
   i.e. the last firmware build's: build the firmware first (pio run), so the
   simulator and the firmware.bin of an update package are the same version.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import find_zig, ARDUINOJSON, OUT, ROOT, SIM  # noqa: E402

VERSIONS = OUT.parent / "versions"
NOT_ON_PC = set()   # every file of the clock's
SIM_SOURCES = ["sim_clock_main", "sim_platform", "sim_web", "sim_wifi", "sim_ota", "sim_heap", "sim_ntp", "sim_net"]


def version():
    src = (ROOT / "src" / "Version.cpp").read_text(encoding="utf-8")
    return re.search(r'FIRMWARE_VERSION\[\] = "([^"]+)"', src).group(1)


def main():
    if not ARDUINOJSON.exists():
        sys.exit("ArduinoJson not found - run `pio run` once so PlatformIO downloads the libraries.")
    zig = find_zig()
    OUT.mkdir(parents=True, exist_ok=True)
    clock = sorted(p for p in (ROOT / "src").glob("*.cpp") if p.stem not in NOT_ON_PC)
    sources = [str(SIM / (s + ".cpp")) for s in SIM_SOURCES] + [str(p) for p in clock]
    flags = ["c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
             "-include", str(SIM / "stubs" / "sim_force.h"), "-include", str(SIM / "clock_stubs" / "clock_force.h"),
             "-I", str(SIM / "clock_stubs"), "-I", str(SIM / "stubs"), "-I", str(SIM), "-I", str(ROOT / "include"),
             "-I", str(ARDUINOJSON),
             "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0", "-DJC_FULL_SIM=1",
             "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument", "-Wno-invalid-offsetof"]
    exe = OUT / "jcclock.exe"
    cmd = [zig] + flags + sources + ["-lws2_32", "-o", str(exe)]
    print("building", exe, "...")
    if subprocess.run(cmd).returncode != 0:
        sys.exit("BUILD FAILED")
    ver = version()
    dest = VERSIONS / ver
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, dest / "jcclock.exe")
    print("OK:", exe, "(version %s, also %s)" % (ver, dest / "jcclock.exe"))


if __name__ == "__main__":
    main()
