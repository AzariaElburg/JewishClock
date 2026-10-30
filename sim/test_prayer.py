"""
Rain/dew prayer slide (משיב הרוח / מוריד הטל), checked in the simulator for
every day from 3 days before to 3 days after each switch day, with the
"days before/after" setting at 0, 1 and 2 - 42 runs, about a minute.

    python sim/test_prayer.py

Expected: with setting N, shown only from N days before to N days after the
switch day; with 0 only on the switch day itself, as the plain phrase.
Exit code 1 on any wrong day or wrong text.
"""
import json
import shutil
import subprocess
import sys
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
WORK = SIM_WORK
EXE = SIM_EXE

# Switch days (Hebrew date, so the civil date moves every year).
SWITCHES = [("משיב הרוח", date(2026, 10, 3), "22 Tishrei 5787"),
            ("מוריד הטל", date(2027, 4, 22), "15 Nisan 5787")]
WHEN = ["מ-מחרתיים", "מ-מחר", "מהיום", "מ-אתמול", "מ-שלשום"]   # lang.cpp prayerSwitchWhen, Hebrew


def expected(phrase, d, window):
    if abs(d) > window:
        return None
    return phrase if window == 0 else phrase + " " + WHEN[d + 2]


def run(fs, day, out):
    cmd = [str(EXE), "--fs", str(fs), "--out", str(out), "--http-cache", str(WORK / "http_cache"),
           "--time", "%s 12:00" % day.isoformat(), "--seconds", "60",
           "--instant-boot"]   # content only - startup timing isn't what this checks
    subprocess.run(cmd, check=True, capture_output=True)
    rows = [l.split("\t", 3) for l in (out / "slides.txt").read_text(encoding="utf-8").splitlines()]
    return [r[3] for r in rows if len(r) == 4]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    base = WORK / "fs"
    if not (base / "config.json").exists():
        sys.exit("run sim/prepare_fs.py first")
    fails = 0
    for phrase, switch_day, label in SWITCHES:
        print("%s - switch day %s (%s)" % (phrase, switch_day, label))
        for window in (0, 1, 2):
            fs = WORK / "test_prayer" / ("fs_%s_w%d" % (switch_day, window))
            shutil.rmtree(fs, ignore_errors=True)
            shutil.copytree(base, fs, dirs_exist_ok=True)
            (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
            cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
            cfg["prayerSwitchDays"] = window
            cfg["debugTimeOffset"] = 0
            (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
            line = []
            for d in range(-3, 4):
                day = switch_day + timedelta(days=d)
                texts = run(fs, day, WORK / "test_prayer" / ("out_%s_w%d_%+d" % (switch_day, window, d)))
                shown = sorted({t for t in texts if t.startswith(phrase)})
                want = expected(phrase, d, window)
                ok = shown == ([want] if want else [])
                fails += not ok
                line.append("%+d:%s" % (d, "ok" if ok else "WRONG"))
                if not ok:
                    print("   WRONG  setting %d, day %+d (%s): expected %r, shown %r" % (window, d, day, want, shown))
            print("  setting %d: %s" % (window, "  ".join(line)))
    print("rain/dew prayer test:", "ALL CORRECT" if not fails else "%d WRONG" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
