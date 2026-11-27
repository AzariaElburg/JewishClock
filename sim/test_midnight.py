"""
Midnight on simulated time (2026-11-21, the user: "midnight is not to break anything - either ignored
or taken into account, not left to chance"). The clock reads one moment per main-loop pass
(NTPManager::beginPass()) - a new day only between passes; these check what the screen then shows:
  1. a plain midnight: no Gregorian date slide after 00:00 says yesterday's date, none before says
     tomorrow's - in both display modes;
  2. Friday night into Shabbat: Friday's candle lighting not shown after 00:00; Shabbat's Havdalah is;
  3. the calendar download asked before midnight, its answer after it (no saved calendar, the
     download held 70 s): after midnight, the right date at once (the clock's own calculation),
     and the downloaded calendar's slides once it arrives - none of yesterday's.
    C:/pio3/jc_clean/venv/Scripts/python sim/test_midnight.py      (after sim/build.py)
"""
import datetime
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clock_facts import build_words  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_midnight")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
fails = []
DATE_SLIDE = re.compile(r"^(\d{1,2}) \S+ (\d{4})$")   # the Gregorian date slide: "21 נובמבר 2026"


def check(what, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  -> %s" % (detail,)))
    if not ok:
        fails.append(what)


def run(name, when, seconds, extra=(), cfg_extra=None):
    """jcsim from `when` for `seconds`: [(datetime shown, text)], what it printed."""
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg.update({"language": "he"})
    cfg.update(cfg_extra or {})
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
    out = WORK / name / "out"
    r = subprocess.run([str(EXE), "--fs", str(fs), "--time", when, "--seconds", str(seconds), "--out", str(out)] + list(extra),
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    rows = []
    f = out / "slides.txt"
    for line in (f.read_text(encoding="utf-8").splitlines() if f.exists() else []):
        p = line.split("\t")
        if len(p) >= 4 and not p[1].startswith("1970"):
            rows.append((datetime.datetime.strptime(p[1], "%Y-%m-%d %H:%M:%S"), p[-1]))
    return rows, r.stdout + r.stderr


def dates_shown(rows):
    """[(when shown, (day, year))] of the Gregorian date slides."""
    out = []
    for t, s in rows:
        for part in s.split(" | "):   # single scroll: every slide in one ticker
            m = DATE_SLIDE.match(part.strip())
            if m:
                out.append((t, (int(m.group(1)), int(m.group(2)))))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")

    print("1. a plain midnight (Tuesday into Wednesday), both display modes")
    day = datetime.date(2026, 12, 1)
    nxt = day + datetime.timedelta(days=1)
    midnight = datetime.datetime.combine(nxt, datetime.time())
    for mode, label in ((0, "alternating"), (1, "single scroll")):
        rows, _ = run("plain_%d" % mode, "%s 23:57:00" % day, 360, ["--instant-boot"], {"rotationMode": mode})
        shown = dates_shown(rows)
        before = [d for t, d in shown if t < midnight]
        after = [d for t, d in shown if t >= midnight]
        check("%s: the date slide before 00:00 is today's" % label, before and all(d == (day.day, day.year) for d in before), before[-3:])
        check("%s: after 00:00 it is tomorrow's - never yesterday's" % label,
              after and all(d == (nxt.day, nxt.year) for d in after), after[:3])

    print("2. Friday night into Shabbat: candle lighting is Friday's, Havdalah Shabbat's")
    fri = datetime.date(2026, 11, 27)
    midnight = datetime.datetime.combine(fri + datetime.timedelta(days=1), datetime.time())
    rows, _ = run("shabbat", "%s 23:56:00" % fri, 480, ["--instant-boot"])
    candles_after = [(t, s) for t, s in rows if t >= midnight and "Candles" in s]
    havdala_after = [(t, s) for t, s in rows if t >= midnight and "Havdala" in s]
    check("Friday's candle lighting not shown after 00:00", not candles_after, candles_after[:2])
    check("Shabbat's Havdalah shown after 00:00", bool(havdala_after), [s for _, s in rows if _ >= midnight][:6])

    print("3. the calendar download asked before midnight, answered after it")
    day = datetime.date(2026, 12, 2)
    nxt = day + datetime.timedelta(days=1)
    midnight = datetime.datetime.combine(nxt, datetime.time())
    rows, said = run("download", "%s 23:59:10" % day, 300, ["--timing", "wifi=1000,ntp=500,main#1=70000,zmanim=1000", "--verbose"])
    words = build_words(EXE, lambda name: check("the clock lists its words for %s (Texts.h)" % name, False))
    urls = [u for (u,) in words.values("calFetching", said)]   # the clock's own words, asked of it
    asked = [m.group(1) for m in (re.search(r"start=(\d{4}-\d\d-\d\d)", u) for u in urls) if m]
    check("the download was asked before midnight (start=%s)" % (asked[:1] or ["?"])[0], bool(asked) and asked[0] == day.isoformat(), asked[:2])
    shown = dates_shown(rows)
    after = [d for t, d in shown if t >= midnight]
    check("after 00:00 the date is tomorrow's at once and stays so (the download still on its way, then in)",
          after and all(d == (nxt.day, nxt.year) for d in after), after[:4])
    late = [s for t, s in rows if t >= midnight + datetime.timedelta(seconds=90)]
    check("... and once it is in, its slides are shown (the parasha)", any("Torah" in s for s in late), late[:6])

    print("\nmidnight test: %s" % ("ALL CORRECT" if not fails else "%d FAILED" % len(fails)))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
