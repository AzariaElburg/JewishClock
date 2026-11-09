"""
The clock's own startup log over USB: restart it (a reset pulse - writes
nothing to the clock) and record its serial output.

    C:/pio3/jc_clean/venv/Scripts/python utils/capture_boot_log.py [--port COM5] [--seconds 25] [--out FILE]

Default out: reference/clock-<date>/boot_serial.log. What it's for: facts only
the real clock knows - e.g. "LittleFS mounted. Free: N bytes" with its files,
to check how the simulator counts flash space (sim/README.md).
"""
import argparse
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--seconds", type=float, default=25)
    ap.add_argument("--out", default=str(ROOT / "reference" / ("clock-" + time.strftime("%Y-%m-%d")) / "boot_serial.log"))
    a = ap.parse_args()
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.2
    s.dtr = False; s.rts = False          # IO0 high = normal boot
    s.open()
    s.rts = True; time.sleep(0.15); s.rts = False   # reset pulse
    data = bytearray()
    end = time.time() + a.seconds
    while time.time() < end:
        data += s.read(4096)
    s.close()
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(data))
    text = data.decode("utf-8", "replace")
    for pat in (r"LittleFS mounted\. Free: \d+ bytes", r"=== JewishClock .* ===", r"FILE: /\S+ \(\d+ bytes\)"):
        hits = re.findall(pat, text)
        print("%-40s %s" % (pat[:40], hits[:3] if pat.startswith("FILE") else hits))
    print("%d bytes -> %s" % (len(data), out))


if __name__ == "__main__":
    main()
