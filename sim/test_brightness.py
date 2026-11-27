"""
Brightness learning on simulated days: builds sim/test_brightness.cpp with
the clock's src/BrightnessManager.cpp (unchanged) and runs it - widening at
once, narrowing after 3 days, the minimum gap, a restart, millis() wrapping,
at most one save an hour. A few seconds.

    python sim/test_brightness.py

Exit code 1 on any wrong result.
"""
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402 - same compiler and flags as the simulator


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    # A measuring run (run_all_tests --coverage sets JC_COV_DIR): built with line counting, under
    # its own name - its counts written at exit to LLVM_PROFILE_FILE (coverage_report.py reads it).
    cov = bool(os.environ.get("JC_COV_DIR"))
    if cov and "--coverage" not in sys.argv:
        sys.argv.append("--coverage")   # build.compiler() looks there
    exe = build.OUT / ("test_brightness_cov.exe" if cov else "test_brightness.exe")
    build.OUT.mkdir(parents=True, exist_ok=True)
    cmd = build.compiler(["c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
           "-include", str(build.SIM / "stubs" / "sim_force.h"),
           "-I", str(build.SIM / "stubs"), "-I", str(build.SIM), "-I", str(build.ROOT / "include"),
           "-I", str(build.ARDUINOJSON), "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0",
           "-Wno-deprecated-declarations"]) + [
           str(build.SIM / "test_brightness.cpp"), str(build.ROOT / "src" / "BrightnessManager.cpp"), str(build.ROOT / "src" / "Tunables.cpp")]
    if not build.link(cmd, exe):
        sys.exit("BUILD FAILED")
    sys.exit(subprocess.run([str(exe)]).returncode)


if __name__ == "__main__":
    main()
