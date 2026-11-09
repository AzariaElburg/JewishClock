"""
A long run of the full simulator in real time (the clock's own day: date
changes, NTP re-syncs, calendar refreshes, log rotation, memory over hours).

    C:/pio3/jc_clean/venv/Scripts/python sim/soak.py --until 06:30 [--every 600]

Watches the work simulator already running (sim_ports.py "dev"): every
--every seconds one line in C:/pio3/jc_clean/sim/soak/<date>.tsv - its time against
a separate NTP query, uptime (a drop = it restarted), PSRAM in use, the screen,
and new ERROR/WARN/watchdog/crash lines in its log. A summary at the end.
"""
import argparse
import json
import socket
import struct
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_ports import urls  # noqa: E402
DEV_HOME, _, DEV_CTL = urls("dev")

OUT = Path("C:/pio3/jc_clean/sim/soak")


def get(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def ntp_now():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(b"\x23" + 47 * b"\0", ("pool.ntp.org", 123))
    d = s.recv(48)
    sec, frac = struct.unpack("!II", d[40:48])
    return sec - 2208988800 + frac / 2 ** 32


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", default="06:30")
    ap.add_argument("--every", type=int, default=600)
    ap.add_argument("--log", default="C:/pio3/jc_clean/sim/clock/run.log")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / (time.strftime("%Y%m%d_%H%M") + ".tsv")
    hh, mm = map(int, a.until.split(":"))
    logpos = Path(a.log).stat().st_size if Path(a.log).exists() else 0
    rows, restarts, problems, last_up = 0, 0, [], None
    with open(out, "w", encoding="utf-8") as f:
        f.write("time\tclock-minus-ntp_s\tuptime_s\tpsram_MB\tversion\tscreen\tnew_problem_lines\n")
        while True:
            now = datetime.now()
            try:
                st = json.loads(get(DEV_HOME + "/api/status"))
                ctl = json.loads(get(DEV_CTL + "/state"))
                diff = st["utcTime"] - ntp_now()
                up = st.get("uptimeMs", 0) / 1000
                if last_up is not None and up < last_up:
                    restarts += 1
                last_up = up
                txt = Path(a.log).read_bytes()[logpos:].decode("utf-8", "replace")
                logpos += len(txt.encode("utf-8"))
                bad = [l for l in txt.splitlines() if any(w in l for w in ("ERROR", "WARN", "watchdog", "crash", "FATAL", "SLOW"))]
                problems += bad
                f.write("%s\t%.2f\t%.0f\t%.2f\t%s\t%s\t%d\n" % (now.strftime("%H:%M"), diff, up, ctl.get("psramUsed", 0) / 1e6,
                                                               st.get("firmware"), ctl.get("screen", "")[:60], len(bad)))
            except Exception as e:
                f.write("%s\tunreachable: %s\n" % (now.strftime("%H:%M"), e))
            f.flush()
            rows += 1
            if (now.hour, now.minute) >= (hh, mm) and now.hour < 12:
                break
            time.sleep(a.every)
    summary = out.with_suffix(".summary.txt")
    summary.write_text("%d samples, %d restart(s), %d problem line(s)\n%s\n" % (rows, restarts, len(problems), "\n".join(problems[:50])),
                       encoding="utf-8")
    print(summary.read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
