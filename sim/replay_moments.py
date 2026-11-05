"""
The date check without the clock: today's simulator against the real
clock's screenshots that moments.py saved (C:/pio3/jc_clean/sim/work/moments/<name>/
NNN.bmp + index.txt - the last run on the clock was 2026-11-04/05, before it
went to the parents).

    C:/pio3/jc_clean/venv/Scripts/python sim/replay_moments.py [--only NAME ...] [--expect-changed CHARS]

Same moments, same settings, same comparison as moments.py; the clock's side
is the saved screenshots. A slide may differ on purpose when the code changed
how it looks since that run - --expect-changed lists characters whose slides
are allowed to differ (default: geresh/gershayim, raised 2026-11-04 after the
last clock run). Everything else must still be identical.
Results: <moment>/replay.txt, summary in C:/pio3/jc_clean/sim/work/moments/replay_summary.txt.
Needs: sim/build.py run first (the code to check).
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import moments  # noqa: E402
from moments import MOMENTS, OUT, WORK, make_fs, run_sim_match, sim_epoch  # noqa: E402

BOOT_TO_CAPTURE_S = 75   # the clock was restarted at the moment, screenshots began ~60-75 s later
ANIMATED = ("/IMG/Cake.bmp", "/IMG/Candles.bmp", "/IMG/MemorialCandle.bmp")   # pictures with a flickering flame
FLAME_MAX_PX = 300       # a flame frame differs in < 100 px (seen 45-86); a real fault is far bigger


def setting_dirs(name):
    d = OUT / name
    if (d / "index.txt").exists():
        return [(d, json.loads((d / "fs" / "config.json").read_text(encoding="utf-8")).get("prayerSwitchDays", 2))]
    return [(d / ("setting%d" % w), w) for w in (0, 1, 2) if (d / ("setting%d" % w) / "index.txt").exists()]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--expect-changed", default="\u05f3\u05f4")
    ap.add_argument("--exe", help="another simulator build (sim/build.py --exe PATH)")
    args = ap.parse_args()
    if args.exe:
        moments.EXE = Path(args.exe)
    base, summary = WORK / "fs", []
    totals = {"OK": 0, "EXPECTED": 0, "MISMATCH": 0}
    for name, when in MOMENTS:
        if args.only and not any(o in name for o in args.only):
            continue
        for outdir, w in setting_dirs(name):
            fs = OUT / name / ("replay_fs_w%d" % w)
            make_fs(base, fs, w)
            rows, _ = run_sim_match(fs, outdir, sim_epoch(fs, when), BOOT_TO_CAPTURE_S + 120)
            same = [r for r in rows if r[1] == "SAME"]
            diff = [r for r in rows if r[1] != "SAME"]
            allowed = [r for r in diff if any(c in r[5] for c in args.expect_changed)]
            # A flame's few pixels (cake, candles) are the animation's phase, not a fault (sim/README.md).
            flame = [r for r in diff if r not in allowed and any(p in r[5] for p in ANIMATED)
                     and r[2].isdigit() and int(r[2]) <= FLAME_MAX_PX]
            bad = [r for r in diff if r not in allowed and r not in flame]
            verdict = "MISMATCH" if bad else ("EXPECTED" if allowed else "OK")
            totals[verdict] += 1
            line = "%s setting %d\t%s\t%d/%d identical%s%s%s" % (
                name, w, verdict, len(same), len(rows),
                ("\tflame phase: %d" % len(flame)) if flame else "",
                ("\tchanged as expected: %d" % len(allowed)) if allowed else "",
                ("\tdiffer: %s" % ["%s %s px %s" % (r[0], r[2], r[5]) for r in bad]) if bad else "")
            (outdir / "replay.txt").write_text(line + "\n", encoding="utf-8")
            summary.append(line); print(line, flush=True)
    summary.append("== %d OK, %d with only the expected changes, %d MISMATCH ==" % (
        totals["OK"], totals["EXPECTED"], totals["MISMATCH"]))
    print(summary[-1])
    (OUT / "replay_summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
