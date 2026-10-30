"""
Checks the simulator against the real clock, pixel for pixel.

    python sim/compare.py [--seconds 40] [--clock <clock-ip>] [--name now]

1. Takes screenshots from the clock (GET /api/debug/screenshot) for --seconds,
   keeping each distinct slide/scroll position once, with the text the clock
   says is on screen (X-Screen-Text) and its scroll position (X-Scroll-X).
2. Runs the simulator at the clock's own "now" (its debug time included) with
   the same settings, events and web files (prepare_fs.py must have run).
3. The simulator compares each screenshot with its own frame showing the same
   text at the same scroll position: C:/pio3/jc_clean/sim/work/compare/<name>/match.txt,
   plus sim_<file>.bmp next to every screenshot that differs.

Prints one line per screenshot and a total; exit code 1 if anything differs.
The clock is tried by name first, then by every --clock address (and the
other way round once one of them answers).
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
WORK = SIM_WORK
EXE = SIM_EXE


class Clock:
    def __init__(self, hosts):
        self.hosts = hosts

    def get(self, path, timeout=10):
        last = None
        for h in list(self.hosts):
            try:
                with urllib.request.urlopen("http://%s%s" % (h, path), timeout=timeout) as r:
                    body, headers = r.read(), r.headers
                if self.hosts[0] != h:      # remember what answered
                    self.hosts.remove(h); self.hosts.insert(0, h)
                return body, headers
            except Exception as e:
                last = e
        raise RuntimeError("clock not reachable (%s): %s" % (", ".join(self.hosts), last))


def capture(clock, outdir, seconds, per_text=4):
    outdir.mkdir(parents=True, exist_ok=True)
    status = json.loads(clock.get("/api/status")[0])
    seen, counts, rows = set(), {}, []
    end = time.time() + seconds
    while time.time() < end:
        try:
            body, h = clock.get("/api/debug/screenshot", timeout=15)
        except RuntimeError as e:
            print("  screenshot failed:", e); time.sleep(1); continue
        text = urllib.parse.unquote(h.get("X-Screen-Text", ""))
        scroll = int(h.get("X-Scroll-X", "0"))
        key = (text, scroll)
        if key in seen or counts.get(text, 0) >= per_text or len(body) < 1000:
            continue
        seen.add(key); counts[text] = counts.get(text, 0) + 1
        name = "%03d.bmp" % len(rows)
        (outdir / name).write_bytes(body)
        rows.append("%s\t%d\t%s" % (name, scroll, text))
    (outdir / "index.txt").write_text("\n".join(rows) + "\n", encoding="utf-8")
    return status, len(rows)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=40)
    ap.add_argument("--clock", action="append", default=[])
    ap.add_argument("--name", default="now")
    ap.add_argument("--sim-seconds", type=float, default=60, help="extra simulated seconds after the capture")
    args = ap.parse_args()
    if not EXE.exists():
        sys.exit("build the simulator first: python sim/build.py")
    if not (WORK / "fs" / "config.json").exists():
        sys.exit("prepare its file system first: python sim/prepare_fs.py")

    clock = Clock(["jewishclock.local"] + args.clock)
    outdir = WORK / "compare" / args.name
    if outdir.exists():
        shutil.rmtree(outdir)
    status, n = capture(clock, outdir, args.seconds)
    print("%d screenshot(s) from the clock (firmware %s)" % (n, status.get("firmware")))
    if not n:
        sys.exit(1)

    # The simulator powers on at the clock's own power-on moment (its time
    # minus its uptime) and goes through the same startup - see sim_main.cpp.
    up_s = status["uptimeMs"] / 1000.0
    start = int(status["utcTime"] - up_s)
    cmd = [str(EXE), "--fs", str(WORK / "fs"), "--out", str(outdir / "sim_slides"),
           "--http-cache", str(WORK / "http_cache"), "--epoch", str(start),
           "--seconds", str(up_s + args.seconds + args.sim_seconds), "--match", str(outdir)]
    subprocess.run(cmd, check=True)

    bad = 0
    for line in (outdir / "match.txt").read_text(encoding="utf-8").splitlines():
        f, result, diff, scroll, nearest, text = line.split("\t", 5)
        if result != "SAME":
            bad += 1
        extra = {"SAME": "", "DIFFERENT": " (%s pixels differ)" % diff,
                 "NO-FRAME-AT-SCROLL": " (scroll %s; nearest simulated %s)" % (scroll, nearest),
                 "TEXT-NEVER-SHOWN": ""}[result]
        print("  %s %-18s %s%s" % (f, result, text, extra))
    print("%s: %d of %d identical" % (args.name, n - bad, n))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
