"""
Measures the clock's startup and download times over several restarts, for
the simulator's timing ranges (sim_platform.cpp). Each restart is at a
different far date, so every one does the location lookup, the main
calendar download and the sunset-times download. Real time is restored at
the end.

    C:/pio3/jc_clean/venv/Scripts/python sim/measure_boot.py --clock <clock-ip> [--runs 8]

Writes C:/pio3/jc_clean/sim/work/boot_times.tsv (one row per restart) and prints min/max.
Needs the USB cable (reset pulse + the clock's log).
"""
import argparse
import re
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import moments as M

STAMP = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
STARTS = {"geo": "Syncing TZ rules in background", "main": "Fetch conditions met. Requesting", "zmanim": "Requesting Zmanim URL"}   # copy-ok: reads the real clock's log, older versions too
ENDS = {"geo": "[Geo] Sync Complete", "main": "HTTP GET completed", "zmanim": "Zmanim gap-fill complete"}


def stamp_s(text):
    m = STAMP.match(text)
    return None if not m else int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000.0


def one_restart(clock, port, when, seconds=45):
    M.wait_ready(clock, min_uptime_s=0)
    clock.post_json("/api/debug/time", {"timestamp": M.sim_epoch(M.WORK / "fs", when)})
    time.sleep(8)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr = False; s.rts = False
    s.open()
    s.reset_input_buffer()
    s.rts = True; time.sleep(0.15); s.rts = False
    t0 = time.time(); buf = b""
    row = {}; starts = {}
    while time.time() - t0 < seconds:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if "reconnected #1" in text and "ap" not in row:   # which access point it joined, at what signal - copy-ok: reads the real clock's log, older versions too
                m = re.search(r"rssi=(-?\d+) ap=([0-9A-Fa-f:]+)", text)
                if m: row["ap"] = "%s(%s)" % (m.group(2)[-5:], m.group(1))
            if "Manifest cache loaded" in text and "setup" not in row:
                row["setup"] = stamp_s(text)         # uptime: end of the clock's setup()
            if "Got IP" in text and "wifi" not in row:
                row["wifi"] = stamp_s(text)          # before the first sync the stamp is the uptime
            if ("First sync" in text or "first time: " in text) and "ntp" not in row:   # firmware before / after 2026-11-17 - copy-ok: reads the real clock's log, older versions too
                row["ntp"] = time.time() - t0       # PC time since the reset pulse (+-0.1 s)
            for k, pat in STARTS.items():
                if pat in text and k not in starts:
                    starts[k] = stamp_s(text)
            for k, pat in ENDS.items():
                if pat in text and k in starts and k not in row and stamp_s(text) is not None:
                    row[k] = stamp_s(text) - starts[k]
    s.close()
    return row


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", action="append", default=[])
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--runs", type=int, default=8)
    args = ap.parse_args()
    clock = M.Clock(args.clock + ["jewishclock.local"])
    keys = ["setup", "wifi", "ntp", "geo", "main", "zmanim"]
    rows = []
    try:
        for i in range(args.runs):
            when = "2027-%02d-15 12:00" % (1 + (i * 5) % 12)   # a different month each time
            row = one_restart(clock, args.port, when)
            rows.append(row)
            print("run %d (%s): %s  ap=%s" % (i + 1, when, "  ".join("%s=%.2f" % (k, row[k]) if k in row else "%s=-" % k for k in keys),
                                               row.get("ap", "-")), flush=True)
    finally:
        M.restore_real_time(clock, args.port, 2)
    out = M.WORK / "boot_times.tsv"
    out.write_text("\t".join(keys) + "\n" + "\n".join("\t".join("%.3f" % r[k] if k in r else "" for k in keys) for r in rows) + "\n")
    for k in keys:
        v = [r[k] for r in rows if k in r]
        if v:
            print("%-7s n=%d  min %.2f s  max %.2f s" % (k, len(v), min(v), max(v)))


if __name__ == "__main__":
    main()
