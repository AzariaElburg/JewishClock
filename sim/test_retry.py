"""
Hebcal out of reach, on the screen simulator's simulated time (jcsim --down): when
the clock tries again (Calendar.cpp retryWaitMs: RETRY_FIRST_MS doubling up to RETRY_MAX_MS,
after the connect timeout - all asked of jcsim --tunables), and that the calendar comes as soon as Hebcal is
back. Half an hour of outage in under a minute; the full simulator's resilience tests check
the first tries on the real download path (resilience.py hebcal_down_at_start).

    python sim/test_retry.py [--only outage|blip|zmanim]      (after sim/build.py)
"""
import shutil
import subprocess
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clock_facts import jcsim_tunables  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_retry")
EXE = Path(sys.argv[sys.argv.index("--exe") + 1] if "--exe" in sys.argv else "C:/pio3/jc_clean/sim/build/jcsim.exe")
SEED = "1"              # the same start-up timing every run (sim/README.md "--seed")
# A fixed moment, far from midnight UTC: the clock rightly fetches the next day's
# calendar when the UTC date changes - run on real time near midnight UTC, that
# counted as an "extra try" (failed 2026-11-14 at 23:55 UTC). A Tuesday at noon.
START = "2026-11-10 12:00"
OUTAGE_S = 1800
# The clock's numbers, asked (the user, 2026-11-19) - filled in main(): the stand-in's connect timeout
# for a service that's down (the library's, sim/stubs/HTTPClient.h), the waits (Calendar.cpp).
CONNECT_S = FIRST_S = MAX_S = None

fails = 0


def check(label, ok, detail=""):
    global fails
    print(("OK   " if ok else "FAIL ") + label + ("" if ok or not detail else "  -- %s" % (detail,)))
    fails += not ok


def run(name, down, seconds):
    """jcsim from power-on with no saved calendar; its downloads: [(seconds, kind, ok)]."""
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    p = subprocess.run([str(EXE), "--fs", str(fs), "--seed", SEED, "--time", START, "--seconds", str(seconds), "--down", down,
                        "--out", str(WORK / name / "out")], capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = []
    for line in p.stderr.splitlines():
        if line.startswith("download "):
            _, ms, res, url = line.split(" ", 3)
            kind = "main" if "hebcal.com/hebcal" in url else "zmanim" if "hebcal.com/zmanim" in url else None
            if kind:
                out.append((int(ms) / 1000, kind, res == "ok"))
    return out


def expected_wait(i):
    return min(FIRST_S * 2 ** i, MAX_S)


def check_spacing(label, tries):
    gaps = [b - a for a, b in zip(tries, tries[1:])]
    bad = [(i, round(g, 1)) for i, g in enumerate(gaps) if not expected_wait(i) + CONNECT_S <= g <= expected_wait(i) + CONNECT_S + 1]
    check("%s: %d tries, waits %s s, then %.0f s" % (label, len(tries), [round(expected_wait(i)) for i in range(3)], MAX_S),
          len(tries) >= 6 and not bad,
          "gaps %s, off: %s" % ([round(g) for g in gaps[:8]], bad[:5]))


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    global CONNECT_S, FIRST_S, MAX_S
    T = jcsim_tunables(EXE)
    CONNECT_S, FIRST_S, MAX_S = T["HTTPCLIENT_DEFAULT_TCP_TIMEOUT"] / 1000, T["RETRY_FIRST_MS"] / 1000, T["RETRY_MAX_MS"] / 1000
    waits_before_max = 0
    while expected_wait(waits_before_max) < MAX_S:
        waits_before_max += 1
    only = sys.argv[sys.argv.index("--only") + 1:] if "--only" in sys.argv else ["outage", "blip", "zmanim"]
    res = {}
    jobs = {"outage": ("hebcal.com@0-%d" % OUTAGE_S, OUTAGE_S + 120),
            "zmanim": ("hebcal.com/zmanim@0-600", 720)}
    threads = [threading.Thread(target=lambda n=n, a=a: res.__setitem__(n, run(n, *a)))
               for n, a in jobs.items() if n in only or (n == "outage" and "blip" in only)]
    for t in threads: t.start()
    for t in threads: t.join()

    if "outage" in only:
        d = res["outage"]
        down = [t for t, k, ok in d if k == "main" and not ok]
        check_spacing("Hebcal down %d min" % (OUTAGE_S // 60), down)
        most = waits_before_max + 1 + int(OUTAGE_S // (MAX_S + CONNECT_S))
        check("... at most %d tries in %d min, not hundreds" % (most, OUTAGE_S // 60), len(down) <= most, len(down))
        back = [t for t, k, ok in d if k == "main" and ok]
        check("Hebcal back: the calendar within %.0f s" % MAX_S, back and back[0] - OUTAGE_S <= MAX_S + 1, back[:1])
        z = [t for t, k, ok in d if k == "zmanim" and ok]
        check("... and its zmanim right after", back and z and 0 <= z[0] - back[0] <= 15, (back[:1], z[:1]))

    if "blip" in only:
        first = next(t for t, k, ok in res["outage"] if k == "main")   # same seed: the same first try
        d = run("blip", "hebcal.com@0-%d" % int(first + 1), 120)
        main_tries = [(t, ok) for t, k, ok in d if k == "main"]
        check("a blip: the first try fails", main_tries and not main_tries[0][1], main_tries[:2])
        check("... the calendar comes %.0f s later (the first wait + the timeout)" % (FIRST_S + CONNECT_S),
              len(main_tries) >= 2 and main_tries[1][1] and main_tries[1][0] - main_tries[0][0] <= FIRST_S + CONNECT_S + 1, main_tries[:2])

    if "zmanim" in only:
        d = res["zmanim"]
        main = [t for t, k, ok in d if k == "main"]
        check("zmanim down: the calendar itself came once (no extra tries)", len(main) == 1 and all(ok for t, k, ok in d if k == "main"), main)
        down = [t for t, k, ok in d if k == "zmanim" and not ok]
        check_spacing("zmanim down 10 min", down)
        z = [t for t, k, ok in d if k == "zmanim" and ok]
        check("zmanim back: within %.0f s" % MAX_S, z and z[0] - 600 <= MAX_S + 1, z[:1])

    print("== %d failed ==" % fails)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
