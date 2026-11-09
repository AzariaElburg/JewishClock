"""
When candle lighting, Havdalah, the fast times, the parasha and Shabbat
Mevarchim show (Calendar Content's From / Until / Tomorrow, 2026-11-08), on
the screen simulator's simulated time - each moment's two minutes of slides in
~3 s. The family's settings and events (Data/), with each case's choices on top;
the slides of those five kinds seen in one round must be exactly the expected
ones. Expectations checked by hand once (2026-11-08, the home city): Shabbat 13-14
Nov 2026, Tisha B'Av / 17 Tammuz / Yom Kippur 5788, Rosh Hashana 5788 on
Shabbat, Shavuot 5787 on Friday, Shabbat Mevarchim Tevet 5787.

    C:/pio3/jc_clean/venv/Scripts/python sim/test_show_windows.py      (after sim/build.py)
"""
import json
import shutil
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_show_windows")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
KINDS = ("Candles", "Havdala", "Torah", "צום", "סעודה", "מברכים")

# choices on top of the family's settings (0/1/2/3 = Config.h ShowFrom / ShowUntil)
CONFIGS = {
    "default": {},
    "evening_attime": {"candleFrom": 1, "candleUntil": 0, "havdalahFrom": 1, "havdalahUntil": 0, "parashaFrom": 1, "parashaUntil": 0},
    "mixed": {"candleFrom": 1, "candleUntil": 1, "candleTomorrow": False, "havdalahFrom": 0, "havdalahUntil": 0, "parashaFrom": 0, "parashaUntil": 1},
    "fast_eve_attime": {"fastFrom": 1, "fastUntil": 0},
    "fast_daybefore_attime": {"fastFrom": 0, "fastUntil": 0},
    "fast_eve_midnight": {"fastFrom": 1, "fastUntil": 1},
    "chag_eve_midnight": {"candleFrom": 1, "candleUntil": 1, "havdalahFrom": 1, "havdalahUntil": 1},
    "mev_own": {"mevarchimFrom": 0, "mevarchimUntil": 1},
}

T, C, H = "Torah |  ", "Candles | ", "Havdala | "
CASES = [   # (config, moment, slides of the five kinds)
    # Shabbat 13-14 Nov 2026: candles 16:22, Havdalah 17:20
    ("default", "2026-11-12 18:30", [T + "תולדות"]),
    ("default", "2026-11-13 21:00", [T + "תולדות", C + "16:22"]),
    ("default", "2026-11-14 17:30", [T + "ויצא", H + "17:20"]),
    ("default", "2026-11-15 00:30", [T + "ויצא"]),
    ("evening_attime", "2026-11-12 12:00", []),
    ("evening_attime", "2026-11-12 16:50", []),
    ("evening_attime", "2026-11-12 18:30", [T + "תולדות", C + "מחר 16:22"]),
    ("evening_attime", "2026-11-13 10:00", [T + "תולדות", C + "16:22"]),
    ("evening_attime", "2026-11-13 16:30", [T + "תולדות"]),
    ("evening_attime", "2026-11-13 21:00", [T + "תולדות", H + "מחר 17:20"]),
    ("evening_attime", "2026-11-14 12:00", [T + "תולדות", H + "17:20"]),
    ("evening_attime", "2026-11-14 17:30", []),
    ("mixed", "2026-11-12 18:30", [C + "16:22"]),
    ("mixed", "2026-11-13 21:00", [T + "תולדות", C + "16:22"]),
    ("mixed", "2026-11-14 12:00", [T + "תולדות", H + "17:20"]),
    ("mixed", "2026-11-14 21:00", [T + "תולדות"]),
    ("mixed", "2026-11-15 00:30", []),
    # Tisha B'Av 5787: from 11 Aug 19:29 to 12 Aug 19:56
    ("default", "2027-08-11 10:00", [T + "ואתחנן", "צום: 19:29 עד מחר 19:56"]),
    ("default", "2027-08-12 20:30", [T + "ואתחנן", "צום: עד 19:56"]),
    ("fast_eve_attime", "2027-08-10 12:00", [T + "ואתחנן"]),
    ("fast_eve_attime", "2027-08-10 21:00", [T + "ואתחנן", "צום: מחר 19:29 עד מחרתיים 19:56"]),
    ("fast_eve_attime", "2027-08-12 12:00", [T + "ואתחנן", "צום: עד 19:56"]),
    ("fast_eve_attime", "2027-08-12 20:30", [T + "ואתחנן"]),
    ("fast_daybefore_attime", "2027-08-10 21:00", [T + "ואתחנן"]),
    ("fast_eve_midnight", "2027-08-12 23:00", [T + "ואתחנן", "צום: עד 19:56"]),
    # 17 Tammuz 5787 (22 Jul 2027), 04:26-20:00
    ("default", "2027-07-21 12:00", [T + "פינחס", "צום: מחר 04:26-20:00"]),
    ("fast_eve_attime", "2027-07-21 12:00", [T + "פינחס"]),
    ("fast_eve_attime", "2027-07-21 21:00", [T + "פינחס", "צום י״ז בתמוז", "צום: מחר 04:26-20:00"]),   # the holiday slide: the Hebrew date is already 17 Tammuz
    ("fast_eve_attime", "2027-07-22 21:00", [T + "פינחס"]),
    ("fast_eve_midnight", "2027-07-22 21:00", [T + "פינחס", "צום: 04:26-20:00"]),
    # Yom Kippur 5788: Erev 10 Oct 2027, candles 17:54; Shabbat before it ends 18:52
    ("default", "2027-10-09 21:00", [H + "18:52"]),
    ("default", "2027-10-10 18:30", [C + "17:54", "סעודה מפסקת עד 17:54"]),
    ("fast_eve_attime", "2027-10-09 17:00", [T + "האזינו", H + "18:52"]),
    ("fast_eve_attime", "2027-10-09 21:00", [H + "18:52", "סעודה מפסקת עד מחר 17:54"]),
    ("fast_eve_attime", "2027-10-10 18:30", [C + "17:54"]),
    # Rosh Hashana 5788 on Shabbat-Sunday: candles Fri 18:06, Sat night 19:01; Havdalah Sun 19:00
    ("default", "2027-10-01 19:00", [C + "18:06"]),
    ("default", "2027-10-02 21:00", [T + "האזינו", C + "19:01"]),
    ("chag_eve_midnight", "2027-09-30 21:00", [C + "מחר 18:06"]),
    ("chag_eve_midnight", "2027-10-01 19:00", [C + "מחר 19:01"]),
    ("chag_eve_midnight", "2027-10-02 21:00", [T + "האזינו", C + "19:01", H + "מחר 19:00"]),
    ("evening_attime", "2027-10-02 21:00", [H + "מחר 19:00"]),
    # Shavuot 5787 on Friday: candles Thu 19:25, Fri 19:26; Havdalah Sat 20:29
    ("chag_eve_midnight", "2027-06-09 22:00", [T + "נשא", C + "מחר 19:25"]),
    ("chag_eve_midnight", "2027-06-10 21:00", [T + "נשא", C + "מחר 19:26"]),
    ("chag_eve_midnight", "2027-06-11 21:00", [T + "נשא", C + "19:26", H + "מחר 20:29"]),
    # Shabbat Mevarchim Tevet 5787 (5 Dec 2026)
    ("default", "2026-12-02 12:00", [T + "וישב", "מברכים חודש טבת"]),
    ("default", "2026-12-05 20:00", [T + "מקץ", H + "17:16"]),
    ("evening_attime", "2026-12-02 12:00", []),
    ("evening_attime", "2026-12-03 20:00", [T + "וישב", C + "מחר 16:16", "מברכים חודש טבת"]),
    ("evening_attime", "2026-12-05 20:00", []),
    ("mev_own", "2026-12-03 20:00", [T + "וישב"]),
    ("mev_own", "2026-12-05 20:00", [T + "מקץ", H + "17:16", "מברכים חודש טבת"]),
]


def run_config(name, moments, results):
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg.update(CONFIGS[name])
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
    for t in moments:
        out = WORK / name / ("out_" + t.replace(" ", "_").replace(":", ""))
        subprocess.run([str(EXE), "--fs", str(fs), "--time", t, "--seconds", "120", "--out", str(out)], capture_output=True)
        seen = []
        p = out / "slides.txt"
        for line in (p.read_text(encoding="utf-8").splitlines() if p.exists() else []):
            text = line.split("\t")[-1].replace("[/IMG/", "").replace(".bmp]", "")
            if any(k in text for k in KINDS) and text not in seen:
                seen.append(text)
        results[(name, t)] = seen


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    by_config = {}
    for name, t, _ in CASES:
        by_config.setdefault(name, []).append(t)
    results = {}
    threads = [threading.Thread(target=run_config, args=(n, m, results)) for n, m in by_config.items()]
    for th in threads: th.start()
    for th in threads: th.join()
    bad = 0
    for name, t, want in CASES:
        got = results.get((name, t), ["(no slides)"])
        ok = sorted(got) == sorted(want)
        bad += not ok
        print("%s %-22s %s  %s" % ("OK  " if ok else "FAIL", name, t, "  |  ".join(got) or "-"))
        if not ok:
            print("     expected: %s" % ("  |  ".join(want) or "-"))
    print("show windows test: " + ("ALL CORRECT" if not bad else "%d of %d WRONG" % (bad, len(CASES))))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
