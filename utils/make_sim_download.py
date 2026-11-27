"""
The clock simulator as a Windows download (2026-11-14): one zip - unpack, double-click
Start.cmd, and a JewishClock runs on the PC with its web page in the browser. No hardware,
no installing, nothing of anybody's: blank settings and events, a made-up home network.

    python utils/make_sim_download.py [--tree DIR]      # DIR: e.g. the clean copy (default: here)

Builds the full simulator (sim/build_clock.py, --tree when given) and packs:
  JewishClock-Simulator/
    jcclock.exe              the clock's whole program (sim/README.md)
    fs/                      its file system: the web page and icons as the clock gets them,
                             the settings and events of a factory-new clock
    state/world.json         the Wi-Fi around it: one home network, "SimHome" (password
                             "simhome123") with internet - the setup page joins it
    Start.cmd                starts it and opens the page
    README.txt
Out: C:/pio3/jc_clean/dist/JewishClock-Simulator-<version>.zip (a release asset of the clean copy).
Real internet: time servers, Hebcal and the place lookup go out for real (curl.exe, part of
Windows 10 and 11).
"""
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import web_files  # noqa: E402
from local_dirs import DIST, LOCAL  # noqa: E402

WORLD = {"networks": [{"ssid": "SimHome", "password": "simhome123", "internet": True, "up": True, "rssi": -55, "channel": 6}]}

START_CMD = r"""@echo off
rem JewishClock simulator: the clock on this PC. Close this window to stop it.
cd /d "%~dp0"
title JewishClock (simulator)
echo JewishClock simulator - its page: http://127.0.0.1:8080 (home network) or http://127.0.0.1:8081 (the clock's own Wi-Fi)
echo A new clock starts in setup mode: on its own Wi-Fi page choose "SimHome", password simhome123.
start "" http://127.0.0.1:8081
jcclock.exe --fs fs --state state
"""

README = """JewishClock simulator
=====================

The JewishClock's own program, running on this PC instead of on the clock: the same
calendar, slides and web page. Only the hardware is pretend - the screen, the Wi-Fi.

Start: double-click Start.cmd. The browser opens the clock's page.

A new clock starts in setup mode, like a real one:
  1. On the page that opens (the clock's own Wi-Fi, http://127.0.0.1:8081) choose the
     home network "SimHome", password simhome123, and Connect.
  2. After a few seconds the clock is on "SimHome": its page is then also at
     http://127.0.0.1:8080. Settings, events, pictures - everything works as on the clock.
  3. The clock's screen: Settings > Advanced shows it live.

It needs the internet (time, the Hebrew calendar). Stop: close the Start.cmd window.
Its settings and events stay in fs/ for the next start; delete the folder for a new clock.

Version: {version}
"""


def main():
    tree = Path(sys.argv[sys.argv.index("--tree") + 1]).resolve() if "--tree" in sys.argv else ROOT
    cmd = [sys.executable, str(ROOT / "sim" / "build_clock.py")] + (["--tree", str(tree)] if tree != ROOT else [])
    out = subprocess.run(cmd, capture_output=True, text=True)
    if "OK:" not in out.stdout:
        sys.exit("simulator build failed:\n" + out.stdout[-2000:] + out.stderr[-2000:])
    exe = Path(out.stdout.split("OK: ")[1].split(" ")[0])
    version = out.stdout.split("version ")[1].split(",")[0].strip()
    work = LOCAL / "sim_download" / "JewishClock-Simulator"
    shutil.rmtree(work.parent, ignore_errors=True)
    (work / "state").mkdir(parents=True)
    shutil.copy2(exe, work / "jcclock.exe")
    web_files.stage(tree / "Data", work / "fs")
    # A factory-new clock: none of the tree's own settings, events, pictures or log
    for name in ("config.json", "events.json"):
        (work / "fs" / name).unlink(missing_ok=True)
    for d in ("images", "custom", "log"):
        shutil.rmtree(work / "fs" / d, ignore_errors=True)
    (work / "state" / "world.json").write_text(json.dumps(WORLD, indent=1), encoding="utf-8")
    (work / "Start.cmd").write_text(START_CMD.replace("\n", "\r\n"), encoding="ascii")
    (work / "README.txt").write_text(README.format(version=version).replace("\n", "\r\n"), encoding="utf-8")
    DIST.mkdir(parents=True, exist_ok=True)
    target = DIST / ("JewishClock-Simulator-%s.zip" % version)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(work.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(work.parent).as_posix())
    print("->", target, "(%d KB)" % (target.stat().st_size // 1024))


if __name__ == "__main__":
    main()
