"""
Every combination of Calendar Content's From / Until / Tomorrow (2026-11-08)
against a model written from the rules, not from the clock's code: for each
moment the model says which candle / Havdalah / parasha / Mevarchim / fast
slide must show, and with which words; the screen simulator (simulated time,
~3 s a moment) must show exactly that.

Candles and Havdalah: 2 From x 2 Until x Tomorrow on/off = 8 each; parasha 5
(All week, Thu evening / Fri midnight x Until Havdalah / midnight); Mevarchim
6 ("Like Parasha" with two parasha choices); fast 4. Moments around two
Shabbatot (13-14 Nov, 4-5 Dec 2026, Mevarchim Tevet), Tisha B'Av, 17 Tammuz
and Erev Yom Kippur 5788 (the home city) - away from the evening's exact minute
(the model takes "evening" as 17:10 in Nov/Dec; צאת is ~17:00-17:07 then).

    C:/pio3/jc_clean/venv/Scripts/python sim/test_show_combos.py      (after sim/build.py)
"""
import json
import shutil
import subprocess
import sys
import threading
from datetime import datetime, date, time, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_show_combos")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")

D = date
CANDLES = [(D(2026, 11, 13), "16:22"), (D(2026, 12, 4), "16:16")]
HAVDALOT = [(D(2026, 11, 14), "17:20"), (D(2026, 12, 5), "17:16")]
SHABBATOT = [  # day, parasha, Mevarchim, Havdalah
    (D(2026, 11, 14), "תולדות", None, "17:20"), (D(2026, 11, 21), "ויצא", None, "17:18"),
    (D(2026, 11, 28), "וישלח", None, "17:17"), (D(2026, 12, 5), "וישב", "מברכים חודש טבת", "17:16"),
    (D(2026, 12, 12), "מקץ", None, "17:17")]
EVENING = {11: time(17, 10), 12: time(17, 10)}   # Nov / Dec: just after צאת הכוכבים
SHABBAT_MOMENTS = ["2026-11-12 12:00", "2026-11-12 18:30", "2026-11-13 10:00", "2026-11-13 16:30", "2026-11-13 21:00",
                   "2026-11-14 12:00", "2026-11-14 17:45", "2026-11-14 21:00", "2026-11-15 00:30",
                   "2026-12-02 12:00", "2026-12-03 18:30", "2026-12-04 10:00", "2026-12-04 21:00",
                   "2026-12-05 12:00", "2026-12-05 17:45", "2026-12-06 00:30"]
FAST_MOMENTS = ["2027-08-10 12:00", "2027-08-10 21:00", "2027-08-11 10:00", "2027-08-11 21:00", "2027-08-12 12:00",
                "2027-08-12 20:30", "2027-08-12 23:00", "2027-08-13 00:30",
                "2027-07-20 23:00", "2027-07-21 12:00", "2027-07-21 21:30", "2027-07-22 03:00", "2027-07-22 12:00",
                "2027-07-22 21:00", "2027-07-23 00:30",
                "2027-10-09 17:00", "2027-10-09 21:00", "2027-10-10 12:00", "2027-10-10 18:30", "2027-10-10 23:00",
                "2027-10-11 00:30"]
# Fasts: begin day / time, end day / time, the day "Day before" counts from, evening start (the Jewish day
# the fast begins in: the evening before), kind
FASTS = [
    dict(begin=(D(2027, 8, 11), "19:29"), end=(D(2027, 8, 12), "19:56"), fast_day=D(2027, 8, 12), evening=datetime(2027, 8, 10, 20, 15)),
    dict(begin=(D(2027, 7, 22), "04:26"), end=(D(2027, 7, 22), "20:00"), fast_day=D(2027, 7, 22), evening=datetime(2027, 7, 21, 20, 30)),
    dict(begin=None, end=(D(2027, 10, 10), "17:54"), fast_day=D(2027, 10, 11), evening=datetime(2027, 10, 9, 18, 52), seuda=True),
]

at = lambda d, hhmm: datetime.combine(d, time(*map(int, hhmm.split(":"))))
midnight = lambda d: datetime.combine(d, time(0, 0))
evening_before = lambda d: datetime.combine(d - timedelta(days=1), EVENING[(d - timedelta(days=1)).month])
WORDS = {1: "מחר ", 2: "מחרתיים "}


def timed(now, items, w, icon):
    """Candles / Havdalah: the slide the rules ask for, or None."""
    for day, t in items:
        start = evening_before(day) if w["from"] == 1 else midnight(day)
        end = at(day, t) if w["until"] == 0 else midnight(day + timedelta(days=1))
        if start <= now < end:
            ahead = (day - now.date()).days
            return icon + (WORDS.get(ahead, "") if w["tomorrow"] else "") + t
    return None


def shabbat_text(now, field, w):
    """Parasha (field 1) / Mevarchim (field 2) with the window w."""
    if w["from"] == 2:   # all week: the nearest Shabbat whose end (צאת, before 17:45) hasn't passed
        for s in SHABBATOT:
            if now < at(s[0], "17:30"):
                return s[field]
        return None
    for s in SHABBATOT:
        day = s[0]
        start = evening_before(day - timedelta(days=1)) if w["from"] == 1 else midnight(day - timedelta(days=1))
        end = at(day, s[3]) if w["until"] == 0 else midnight(day + timedelta(days=1))
        if start <= now < end and s[field]:
            return s[field]
    return None


def fast_text(now, w):
    for f in FASTS:
        start = f["evening"] if w["from"] == 1 else midnight(f["fast_day"] - timedelta(days=1))
        end_day, end_t = f["end"]
        end = at(end_day, end_t) if w["until"] == 0 else midnight(end_day + timedelta(days=1))
        if not (start <= now < end):
            continue
        today = now.date()
        e_in = (end_day - today).days
        if f.get("seuda"):
            return "סעודה מפסקת עד " + WORDS.get(e_in, "") + end_t
        b_day, b_t = f["begin"]
        b_in = (b_day - today).days
        if b_in < 0:
            return "צום: עד " + WORDS.get(e_in, "") + end_t
        if b_in == e_in:
            return "צום: " + WORDS.get(b_in, "") + b_t + "-" + end_t
        return "צום: " + WORDS.get(b_in, "") + b_t + " עד " + WORDS.get(e_in, "") + end_t
    return None


def expected(now, c):
    want = []
    cand = timed(now, CANDLES, c["candle"], "Candles | ")
    hav = timed(now, HAVDALOT, c["havdalah"], "Havdala | ")
    par = shabbat_text(now, 1, c["parasha"])
    mw = c["parasha"] if c["mevarchim"]["from"] == 3 else c["mevarchim"]
    mev = shabbat_text(now, 2, mw)
    for x in (cand, hav, ("Torah |  " + par) if par else None, mev):
        if x:
            want.append(x)
    return want


def seen_slides(lines, kinds):
    seen = []
    for line in lines:
        text = line.split("\t")[-1].replace("[/IMG/", "").replace(".bmp]", "")
        if any(text.startswith(k) or k in text for k in kinds) and text not in seen:
            seen.append(text)
    return seen


def config_keys(c):
    out = {}
    for k, w in c.items():
        out[k + "From"] = w["from"]
        out[k + "Until"] = w["until"]
        if "tomorrow" in w:
            out[k + "Tomorrow"] = w["tomorrow"]
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    timed_combos = [dict(zip(("from", "until", "tomorrow"), (f, u, t))) for f in (0, 1) for u in (0, 1) for t in (True, False)]
    parasha = [dict(**{"from": 2, "until": 0})] + [dict(**{"from": f, "until": u}) for f in (1, 0) for u in (0, 1)]
    mevarchim = [dict(**{"from": 3, "until": 0}), dict(**{"from": 2, "until": 0})] + [dict(**{"from": f, "until": u}) for f in (1, 0) for u in (0, 1)]
    jobs = []
    for i in range(8):   # every candle and Havdalah combination; parasha and Mevarchim cycle through theirs
        c = {"candle": timed_combos[i], "havdalah": timed_combos[(i + 3) % 8], "parasha": parasha[i % 5], "mevarchim": mevarchim[i % 6]}
        jobs.append(("shabbat%d" % i, c, SHABBAT_MOMENTS, ("Candles", "Havdala", "Torah", "מברכים"), lambda now, c=c: expected(now, c)))
    for f in (0, 1):
        for u in (0, 1):
            c = {"fast": {"from": f, "until": u}}
            jobs.append(("fast_f%d_u%d" % (f, u), c, FAST_MOMENTS, ("צום:", "סעודה"),
                         lambda now, w=c["fast"]: [x for x in [fast_text(now, w)] if x]))
    results = {}

    def run(name, c, moments):
        fs = WORK / name / "fs"
        shutil.rmtree(WORK / name, ignore_errors=True)
        shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
        (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
        cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
        cfg.update(config_keys(c))
        (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
        for t in moments:
            out = WORK / name / ("out_" + t.replace(" ", "_").replace(":", ""))
            subprocess.run([str(EXE), "--fs", str(fs), "--time", t, "--seconds", "120", "--out", str(out)], capture_output=True)
            p = out / "slides.txt"
            results[(name, t)] = p.read_text(encoding="utf-8").splitlines() if p.exists() else []

    threads = [threading.Thread(target=run, args=(n, c, m)) for n, c, m, _, _ in jobs]
    for th in threads: th.start()
    for th in threads: th.join()
    bad = total = 0
    for name, c, moments, kinds, model in jobs:
        print("== %s %s" % (name, json.dumps(c, ensure_ascii=False)))
        for t in moments:
            got = seen_slides(results[(name, t)], kinds)
            want = model(datetime.strptime(t, "%Y-%m-%d %H:%M"))
            ok = sorted(got) == sorted(want)
            total += 1
            bad += not ok
            if not ok:
                print("   FAIL %s  shown: %s\n                          expected: %s" % (t, " | ".join(got) or "-", " | ".join(want) or "-"))
    print("show combinations test: " + ("ALL CORRECT (%d moments)" % total if not bad else "%d of %d WRONG" % (bad, total)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
