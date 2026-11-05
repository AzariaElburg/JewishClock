"""
Every slide the clock would show over a period, day by day - for reading
through in one go to spot wrong or missing content (no clock needed).

    python sim/sweep.py [--from 2026-10-18] [--days 365] [--times 12:00 21:00]

Writes C:/pio3/jc_clean/sim/work/sweep/slides.tsv: date, time, slide text (each distinct
slide once per moment). The calendar data carries over from day to day as
on the clock, so Hebcal is asked only when the data runs out.
"""
import argparse
import shutil
import subprocess
import sys
from datetime import date, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
WORK = SIM_WORK


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="start", default=date.today().isoformat())
    ap.add_argument("--days", type=int, default=365)
    ap.add_argument("--times", nargs="+", default=["12:00", "21:00"])
    ap.add_argument("--exe", default=str(SIM_EXE))
    args = ap.parse_args()
    args.exe = str(Path(args.exe).resolve())
    out = WORK / "sweep"
    fs = out / "fs"
    shutil.rmtree(fs, ignore_errors=True)
    shutil.copytree(WORK / "fs", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    rows = []
    d0 = date.fromisoformat(args.start)
    for i in range(args.days):
        day = d0 + timedelta(days=i)
        for t in args.times:
            run = out / "run"
            subprocess.run([args.exe, "--fs", str(fs), "--out", str(run), "--http-cache", str(WORK / "http_cache"),
                            "--time", "%s %s" % (day.isoformat(), t), "--seconds", "45",
                            "--instant-boot"],   # content only - startup timing isn't what this checks
                           check=True, capture_output=True)
            texts = []
            for line in (run / "slides.txt").read_text(encoding="utf-8").splitlines():
                text = line.split("\t", 3)[3]
                if text not in texts:
                    texts.append(text)
            rows += ["%s\t%s\t%s" % (day.isoformat(), t, x) for x in texts]
        if i % 30 == 0:
            print(day, flush=True)
    (out / "slides.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print("%d slides -> %s" % (len(rows), out / "slides.tsv"))


if __name__ == "__main__":
    main()
