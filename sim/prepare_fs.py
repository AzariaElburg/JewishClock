"""
Fills the simulator's file system (C:/pio3/jc_clean/sim/work/fs) the way `uploadfs` fills the
clock's: a copy of Data/, with the settings and personal events taken live
from the clock (GET /api/backup) so the simulator shows what the clock shows.

    python sim/prepare_fs.py                 # settings from the clock
    python sim/prepare_fs.py --local         # settings from Data/ as they are
    python sim/prepare_fs.py --clock <clock-ip>

The clock is tried by name first, then by any address given with --clock.
"""
import argparse
import json
import shutil
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
FS = SIM_WORK / "fs"


def fetch_backup(hosts):
    for h in hosts:
        try:
            with urllib.request.urlopen("http://%s/api/backup" % h, timeout=10) as r:
                data = json.loads(r.read().decode("utf-8"))
            if isinstance(data.get("config"), dict) and data["config"].get("wifiSSID") is not None:
                print("settings and events from", h)
                return data
        except Exception as e:  # try the next address
            print("  %s: %s" % (h, e))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--local", action="store_true", help="use Data/config.json and events.json as they are")
    ap.add_argument("--clock", action="append", default=[], help="extra address to try")
    args = ap.parse_args()

    # Windows sometimes holds a just-emptied folder for a moment: clear what
    # can be cleared, then copy over whatever is left.
    shutil.rmtree(FS, ignore_errors=True)
    shutil.copytree(ROOT / "Data", FS, dirs_exist_ok=True)
    if not args.local:
        backup = fetch_backup(["jewishclock.local"] + args.clock)
        if not backup:
            sys.exit("could not reach the clock - use --clock ADDRESS or --local")
        (FS / "config.json").write_text(json.dumps(backup["config"], ensure_ascii=False, indent=2), encoding="utf-8")
        (FS / "events.json").write_text(json.dumps(backup["events"], ensure_ascii=False, indent=2), encoding="utf-8")
    # The clock keeps downloaded calendar data in its file system; start clean
    # so the simulator fetches its own, exactly as a freshly booted clock would.
    (FS / "jewish_calendar_data.json").unlink(missing_ok=True)   # Calendar.cpp CACHE_PATH
    print("ready:", FS)


if __name__ == "__main__":
    main()
