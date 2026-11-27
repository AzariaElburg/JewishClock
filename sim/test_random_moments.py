"""
A few truly random moments on the screen simulator (2026-11-20, the user: "a very small amount of
really random dates, just for the chance of catching some strange bug") - after all the hand-picked
significant dates the other tests check. Each run: 3 moments between 2026 and 2099, any time of
day, from the run's seed (JC_TEST_SEED - printed; give it back to repeat). Only what must hold at
ANY moment is checked:
  - slides are shown, and the clock doesn't crash;
  - the Gregorian date slide shows that day's number and year;
  - the Hebrew year shown is that day's (Hebcal's converter: before or after sunset - the clock
    changes the date at nightfall);
  - nothing reads 1970, and no year shows as "?".

    C:/pio3/jc_clean/venv/Scripts/python sim/test_random_moments.py [--n 3]      (after sim/build.py)
"""
import datetime
import json
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clock_facts import test_random  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_random_moments")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
fails = []


def check(what, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  -> %s" % (detail,)))
    if not ok:
        fails.append(what)


def hebcal_years(d):
    """The Hebrew year as Hebcal writes it (letters only), for the day and its evening."""
    out = set()
    for gs in ("off", "on"):
        url = "https://www.hebcal.com/converter?cfg=json&gy=%d&gm=%d&gd=%d&g2h=1&gs=%s" % (d.year, d.month, d.day, gs)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "JewishClock-tests"}), timeout=30) as r:
            heb = json.loads(r.read())["hebrew"]
        out.add(letters(heb.split()[-1]))
    return out


def letters(s):
    """Hebrew letters only (no geresh/gershayim, no niqqud)."""
    return "".join(ch for ch in s if "\u05d0" <= ch <= "\u05ea")


def run(name, when):
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    out = WORK / name / "out"
    p = subprocess.run([str(EXE), "--fs", str(fs), "--time", when, "--seconds", "120", "--out", str(out)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    texts = []
    f = out / "slides.txt"
    for line in (f.read_text(encoding="utf-8").splitlines() if f.exists() else []):
        parts = line.split("\t")
        if len(parts) >= 4 and not parts[1].startswith("1970"):
            texts.append(parts[-1])
    return p.returncode, texts


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    n = int(sys.argv[sys.argv.index("--n") + 1]) if "--n" in sys.argv else 3
    rnd = test_random("random moments")
    first, last = datetime.date(2026, 1, 1), datetime.date(2099, 12, 31)
    for i in range(n):
        d = first + datetime.timedelta(days=rnd.randrange((last - first).days + 1))
        when = "%s %02d:%02d" % (d.isoformat(), rnd.randrange(24), rnd.randrange(60))
        print("%d. %s" % (i + 1, when))
        code, texts = run("m%d" % i, when)
        check("the clock runs (exit %s) and shows slides (%d)" % (code, len(texts)), code == 0 and len(texts) >= 3, texts[:4])
        flat = " | ".join(texts)
        check("the Gregorian date: day %d and %d on a slide" % (d.day, d.year),
              any(str(d.year) in t and str(d.day) in t.replace(str(d.year), "") for t in texts), texts[:6])
        years = hebcal_years(d)
        check("the Hebrew year is the day's (Hebcal: %s)" % " / ".join(sorted(years)),
              any(y and y in letters(t) for t in texts for y in years), texts[:6])
        check("nothing reads 1970 or a '?' year", "1970" not in flat and "?" not in flat, flat[:200])
    print("random moments test: " + ("ALL CORRECT" if not fails else "%d FAILED" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
