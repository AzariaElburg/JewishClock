"""
Rain/dew prayer slide, checked in the simulator: both switches (משיב הרוח /
מוריד הטל, and the request ותן טל ומטר / ותן ברכה), every day from 3 days
before to 3 days after each switch day at noon, with the "days before/after"
setting at 0, 1 and 2; plus the evening before each switch day (after צאת),
nusach Sefard and abroad. About 80 runs, a few minutes.

    python sim/test_prayer.py [--only LABEL ...]   (labels: see cases(); six cases at a time)

Expected (decided 2026-11-02), all in civil days (midnight to midnight):
- משיב הרוח (22 Tishrei): 2 days before "ממחרתיים", the day before "ממחר",
  on the day (from midnight) "ממוסף", after it the phrase alone.
- מוריד הטל (15 Nisan): the same, but nothing before its eve.
- The request switches at the first weekday Maariv: the evening of 6
  Cheshvan in Israel, of 4/5 December abroad; at Pesach the evening after
  Yom Tov (Israel 15 Nisan, abroad 16) - never shown before that day. Before
  it "ממחרתיים בערב", "ממחר בערב", on its day "מהערב" (also at setting
  0), after it the phrase alone. An evening starting Shabbat moves to
  Motzei Shabbat.
- One line per switch; one for both when they say the same ("מוריד הטל,
  ותן ברכה" after Pesach). Ashkenazim abroad say "אין משיב הרוח" at Pesach
  instead of מוריד הטל, saying "בלי משיב הרוח". Sefard: "ברך עלינו" / "ברכנו".
Exit code 1 on any wrong text.
"""
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
WORK = SIM_WORK
EXE = SIM_EXE

FROM = ["ממוסף", "ממחר", "ממחרתיים"]          # lang.cpp, Hebrew
EVE = ["מהערב", "ממחר בערב", "ממחרתיים בערב"]
PRAYER = re.compile("הרוח|הטל|ומטר|ברכה|ברכנו|עלינו")

ATZERET = date(2027, 10, 23)    # 22 Tishrei 5788
PESACH = date(2027, 4, 22)      # 15 Nisan 5787, a Thursday: abroad 16 Nisan is Friday
REQUEST_IL = date(2027, 11, 6)  # its evening opens 7 Cheshvan 5788
REQUEST_ABROAD = date(2027, 12, 5)   # Maariv of 5 Dec 2027 (2028 is a leap year)
REQUEST_ABROAD_FRIDAY = date(2026, 12, 4)   # Friday: moves to Motzei Shabbat, 5 Dec


def musaf_part(d, w, phrase, before):   # d = civil day - the switch day
    if d < -before or d > w:
        return None
    return phrase, (FROM[-d] if d <= 0 else "")


def evening_part(d, w, phrase, before):   # d = civil day - the day whose evening switches
    if d < -before or d > w:
        return None
    return phrase, (EVE[-d] if d <= 0 else "")


def lines(p, r):   # as MessageAggregator.cpp's prayerTexts()
    out = []
    if p and r and p[1] == r[1]:
        return [" ".join(x for x in (p[0] + ", " + r[0], p[1]) if x)]
    for part in (p, r):
        if part:
            out.append(" ".join(x for x in part if x))
    return out


def mention(d, w):
    return lines(musaf_part(d, w, "משיב הרוח", w), None)


def pesach(d, w, dew="מוריד הטל", ask="ותן ברכה", r_day=0):   # r_day: its request day, from 15 Nisan
    return lines(musaf_part(d, w, dew, min(w, 1)) if dew else None, evening_part(d - r_day, w, ask, 0))


def request(d, w, ask="ותן טל ומטר"):
    return lines(None, evening_part(d, w, ask, w))


def cases():
    for w in (0, 1, 2):
        for d in range(-3, 4):
            yield ("atzeret", w, {}, ATZERET + timedelta(days=d), "12:00", mention(d, w))
            yield ("pesach", w, {}, PESACH + timedelta(days=d), "12:00", pesach(d, w))
            yield ("request IL", w, {}, REQUEST_IL + timedelta(days=d), "12:00", request(d, w))
    for w in (0, 2):
        # after צאת: the clock shows the next Hebrew day already - no change
        yield ("atzeret eve", w, {}, ATZERET - timedelta(days=1), "20:00", mention(-1, w))
        yield ("pesach eve", w, {}, PESACH - timedelta(days=1), "20:30", pesach(-1, w))
        yield ("pesach evening", w, {}, PESACH, "20:30", pesach(0, w))
        yield ("request IL evening", w, {}, REQUEST_IL, "19:30", request(0, w))
    sefard = {"nusach": 1}
    abroad = {"inIsrael": False}
    yield ("sefard request", 2, sefard, REQUEST_IL, "12:00", request(0, 2, ask="ברך עלינו"))
    yield ("sefard pesach", 2, sefard, PESACH, "12:00", pesach(0, 2, ask="ברכנו"))
    for d in (-2, -1, 0, 1):
        yield ("abroad request", 2, abroad, REQUEST_ABROAD + timedelta(days=d), "12:00", request(d, 2))
    for d in (0, 1, 2):   # Friday 4 Dec 2026: from Motzei Shabbat
        yield ("abroad request friday", 2, abroad, REQUEST_ABROAD_FRIDAY + timedelta(days=d), "12:00", request(d - 1, 2))
    for d in range(-1, 5):   # 16 Nisan is Friday: ותן ברכה from Motzei Shabbat (17 Nisan)
        yield ("abroad ashkenaz pesach", 2, abroad, PESACH + timedelta(days=d), "12:00", pesach(d, 2, dew="בלי משיב הרוח", r_day=2))
        yield ("abroad sefard pesach", 2, {**abroad, **sefard}, PESACH + timedelta(days=d), "12:00",
               pesach(d, 2, ask="ברכנו", r_day=2))


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
    # Not ignore_errors: copies of a read-only base folder are read-only, and
    # rmtree then left them silently - the next run failed copying over them.
    def clear_readonly(func, path, _):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    if root.exists():
        shutil.rmtree(root, onerror=clear_readonly)
    # --only LABEL ...: just those cases (e.g. --only "abroad request"); six simulators at a time (2026-11-11)
    only = sys.argv[sys.argv.index("--only") + 1:] if "--only" in sys.argv else None
    todo = [(i, c) for i, c in enumerate(cases()) if not only or c[0] in only]
    if not todo:
        sys.exit("no such case - labels: %s" % ", ".join(sorted({c[0] for c in cases()})))

    def one(item):
        i, (label, w, extra, day, hhmm, want) = item
        fs = root / ("fs%03d" % i)
        shutil.copytree(base, fs)
        (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
        cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
        cfg.update({"prayerSwitchDays": w, "debugTimeOffset": 0, "nusach": 0, "inIsrael": True})
        cfg.update(extra)
        (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
        texts = run(fs, "%s %s" % (day.isoformat(), hhmm), root / ("out%03d" % i))
        return sorted({p.strip() for t in texts for p in t.split(" | ") if PRAYER.search(p)})

    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(6) as pool:
        shown_all = list(pool.map(one, todo))
    fails = 0
    for (i, (label, w, extra, day, hhmm, want)), shown in zip(todo, shown_all):
        ok = shown == sorted(want)
        fails += not ok
        print("%-5s %-22s setting %d  %s %s  %s" % ("ok" if ok else "WRONG", label, w, day, hhmm,
                                                    " | ".join(shown)))
        if not ok:
            print("      expected %r" % want)
    print("rain/dew prayer test:", "ALL CORRECT" if not fails else "%d WRONG" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
