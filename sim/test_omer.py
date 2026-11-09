"""
The Omer's Compact wording (omerMode 4, 2026-11-06) on all 49 days: the
fullest of "23 = 3 שבועות + 2 ימים לעומר" / without "לעומר" / "23=3 שבועות+2 ימים"
that fits the window, standing still. Days 1-6 "5 לעומר", whole weeks without
"+ 0 ימים".

    C:/pio3/jc_clean/venv/Scripts/python sim/test_omer.py      (after sim/prepare_fs.py)
"""
import json
import shutil
import subprocess
import sys
from datetime import date, timedelta
from pathlib import Path

WORK = Path("C:/pio3/jc_clean/sim/work")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
DAY1 = date(2027, 4, 23)   # 16 Nisan 5787: the Omer's first day (counted the night before)


def candidates(d):
    if d < 7:
        return [f"{d} לעומר"]
    w, r = divmod(d, 7)
    weeks = f"{w} {'שבוע' if w == 1 else 'שבועות'}"
    days = f"{r} {'יום' if r == 1 else 'ימים'}" if r else ""
    spaced = f"{d} = {weeks}" + (f" + {days}" if r else "")
    tight = f"{d}={weeks}" + (f"+{days}" if r else "")
    return [spaced + " לעומר", spaced, tight]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    base = WORK / "fs"
    if not (base / "config.json").exists():
        sys.exit("run sim/prepare_fs.py first")
    root = WORK / "test_omer"
    shutil.rmtree(root, ignore_errors=True)
    fs = root / "fs"
    shutil.copytree(base, fs)
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg["omerMode"] = 4
    cfg["contentFrequency"]["omer"] = 1
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
    # The window's width and every candidate's, as the clock measures them.
    lines = [c for d in range(1, 50) for c in candidates(d)]
    (root / "measure.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    m = subprocess.run([str(EXE), "--measure", str(root / "measure.txt")], capture_output=True, text=True, encoding="utf-8")
    width = {}
    win = 0
    for row in m.stdout.splitlines():
        px, win_s, text = row.split("\t", 2)
        width[text] = int(px)
        win = int(win_s)
    bad = 0
    for d in range(1, 50):
        cands = candidates(d)
        want = next((c for c in cands if width[c] <= win), cands[-1])
        out = root / ("day%02d" % d)
        when = (DAY1 + timedelta(days=d - 1)).isoformat() + " 12:00"
        subprocess.run([str(EXE), "--fs", str(fs), "--out", str(out), "--http-cache", str(WORK / "http_cache"),
                        "--time", when, "--seconds", "30", "--instant-boot"], check=True, capture_output=True)
        rows = [l.split("\t", 3) for l in (out / "slides.txt").read_text(encoding="utf-8").splitlines()]
        shown = [(int(r[2]), r[3]) for r in rows if len(r) == 4 and r[3] in cands]
        ok = bool(shown) and shown[0][1] == want and shown[0][0] >= 0
        bad += not ok
        print("%s day %2d  %-34s %3d px of %d%s" % ("OK " if ok else "BAD", d, shown[0][1] if shown else "(no Omer slide)",
                                                   width[want], win, "" if ok else "   want: " + want))
    print("omer compact test: " + ("ALL CORRECT" if not bad else "%d WRONG" % bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
