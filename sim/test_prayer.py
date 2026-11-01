"""
Rain/dew prayer slide, checked in the simulator: both switches (משיב הרוח /
מוריד הטל, and the request ותן טל ומטר / ותן ברכה), every day from 3 days
before to 3 days after each switch day at noon, with the "days before/after"
setting at 0, 1 and 2; plus the evening before each switch day (after צאת),
nusach Sefard and abroad. About 70 runs, a minute or two.

    python sim/test_prayer.py

Expected (decided 2026-11-01):
- משיב הרוח (22 Tishrei): N days before to N after; on the day "ממוסף", from
  צאת the evening before "ממוסף מחר", other days "מ-מחר", "מ-אתמול", ...
- Pesach (15 Nisan): nothing before; both switch at Musaf, one line:
  "מוריד הטל, ותן ברכה ממוסף". Ashkenazim abroad say nothing in summer: only
  "ותן ברכה ...". Sefard: "ברכנו".
- The request (Maariv opening 7 Cheshvan in Israel, early December abroad):
  before it "מ-מחרתיים בערב", "מ-מחר בערב", "מהערב" (the evening itself is
  always shown, also at 0), from it "מהיום", "מ-אתמול", ...
Exit code 1 on any wrong text.
"""
import json
import re
import shutil
import subprocess
import sys
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
WORK = SIM_WORK
EXE = SIM_EXE

WHEN = ["מ-מחרתיים", "מ-מחר", "מהיום", "מ-אתמול", "מ-שלשום"]   # lang.cpp, Hebrew
EVE = ["מהערב", "מ-מחר בערב", "מ-מחרתיים בערב"]
MUSAF, MUSAF_TOMORROW = "ממוסף", "ממוסף מחר"
PRAYER = re.compile("הרוח|הטל|ומטר|ברכה|ברכנו|עלינו")

ATZERET = date(2027, 10, 23)    # 22 Tishrei 5788
PESACH = date(2027, 4, 22)      # 15 Nisan 5787
REQUEST_IL = date(2027, 11, 7)  # 7 Cheshvan 5788 - its Maariv is on the evening of 6 Nov
REQUEST_ABROAD = date(2027, 12, 6)   # Maariv of 5 Dec 2027 (2028 is a leap year)


def mention(d, w):
    if abs(d) > w:
        return None
    return "משיב הרוח " + (MUSAF if d == 0 else WHEN[d + 2])


def pesach(d, w, dew="מוריד הטל, ", ask="ותן ברכה"):
    if d < 0 or d > w:
        return None
    return dew + ask + " " + (MUSAF if d == 0 else WHEN[d + 2])


def request(d, w, ask="ותן טל ומטר"):   # d = civil day - the Hebrew day the request starts
    if d < 0:
        evenings = -1 - d
        return ask + " " + EVE[evenings] if evenings <= w else None
    return ask + " " + WHEN[d + 2] if d <= w else None


def cases():
    for w in (0, 1, 2):
        for d in range(-3, 4):
            yield ("atzeret", w, {}, ATZERET + timedelta(days=d), "12:00", mention(d, w))
            yield ("pesach", w, {}, PESACH + timedelta(days=d), "12:00", pesach(d, w))
            yield ("request IL", w, {}, REQUEST_IL + timedelta(days=d), "12:00", request(d, w))
    for w in (0, 2):
        # after צאת on the evening before: the clock shows the switch day already
        yield ("atzeret eve", w, {}, ATZERET - timedelta(days=1), "20:00", "משיב הרוח " + MUSAF_TOMORROW)
        yield ("pesach eve", w, {}, PESACH - timedelta(days=1), "20:30", "מוריד הטל, ותן ברכה " + MUSAF_TOMORROW)
        yield ("request IL eve", w, {}, REQUEST_IL - timedelta(days=1), "19:30", "ותן טל ומטר " + WHEN[2])
    sefard = {"nusach": 1}
    abroad = {"inIsrael": False}
    yield ("sefard request", 2, sefard, REQUEST_IL - timedelta(days=1), "12:00", "ברך עלינו " + EVE[0])
    yield ("sefard pesach", 2, sefard, PESACH, "12:00", pesach(0, 2, ask="ברכנו"))
    for d in (-2, -1, 0, 1):
        yield ("abroad request", 2, abroad, REQUEST_ABROAD + timedelta(days=d), "12:00", request(d, 2))
    yield ("abroad ashkenaz pesach", 2, abroad, PESACH, "12:00", pesach(0, 2, dew=""))
    yield ("abroad ashkenaz pesach", 2, abroad, PESACH + timedelta(days=1), "12:00", pesach(1, 2, dew=""))
    yield ("abroad sefard pesach", 2, {**abroad, **sefard}, PESACH, "12:00", pesach(0, 2, ask="ברכנו"))


def run(fs, when, out):
    cmd = [str(EXE), "--fs", str(fs), "--out", str(out), "--http-cache", str(WORK / "http_cache"),
           "--time", when, "--seconds", "60",
           "--instant-boot"]   # content only - startup timing isn't what this checks
    subprocess.run(cmd, check=True, capture_output=True)
    rows = [l.split("\t", 3) for l in (out / "slides.txt").read_text(encoding="utf-8").splitlines()]
    return [r[3] for r in rows if len(r) == 4]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    base = WORK / "fs"
    if not (base / "config.json").exists():
        sys.exit("run sim/prepare_fs.py first")
    root = WORK / "test_prayer"
    shutil.rmtree(root, ignore_errors=True)
    fails = 0
    for i, (label, w, extra, day, hhmm, want) in enumerate(cases()):
        fs = root / ("fs%03d" % i)
        shutil.copytree(base, fs)
        (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
        cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
        cfg.update({"prayerSwitchDays": w, "debugTimeOffset": 0, "nusach": 0, "inIsrael": True})
        cfg.update(extra)
        (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
        texts = run(fs, "%s %s" % (day.isoformat(), hhmm), root / ("out%03d" % i))
        shown = sorted({p.strip() for t in texts for p in t.split(" | ") if PRAYER.search(p)})
        ok = shown == ([want] if want else [])
        fails += not ok
        print("%-5s %-22s setting %d  %s %s  %s" % ("ok" if ok else "WRONG", label, w, day, hhmm,
                                                    shown[0] if len(shown) == 1 else shown))
        if not ok:
            print("      expected %r" % want)
    print("rain/dew prayer test:", "ALL CORRECT" if not fails else "%d WRONG" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
