"""
Brightness learning on simulated days: builds sim/test_brightness.cpp with
the clock's src/BrightnessManager.cpp (unchanged) and runs it - widening at
once, narrowing after 3 days, the minimum gap, a restart, millis() wrapping,
at most one save an hour. A few seconds.

    python sim/test_brightness.py

Exit code 1 on any wrong result.
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402 - same compiler and flags as the simulator


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    exe = build.OUT / "test_brightness.exe"
    build.OUT.mkdir(parents=True, exist_ok=True)
    cmd = [build.find_zig(), "c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
           "-include", str(build.SIM / "stubs" / "sim_force.h"),
           "-I", str(build.SIM / "stubs"), "-I", str(build.SIM), "-I", str(build.ROOT / "include"),
           "-I", str(build.ARDUINOJSON), "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0",
           "-Wno-deprecated-declarations",
           str(build.SIM / "test_brightness.cpp"), str(build.ROOT / "src" / "BrightnessManager.cpp"),
           "-o", str(exe)]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("BUILD FAILED")
    sys.exit(subprocess.run([str(exe)]).returncode)


if __name__ == "__main__":
    main()
