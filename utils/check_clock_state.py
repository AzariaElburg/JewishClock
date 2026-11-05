"""
The clock's state as it should be handed over - one look, every point.

    C:/pio3/jc_clean/venv/Scripts/python utils/check_clock_state.py [--clock http://jewishclock.local] [--version 2.1.0]

Firmware version, time synced and the debug offset 0, the networks in order,
display mode, events and pictures present, update state ok, the guide and
manual served and stamped with the running version. Exit 1 if anything is off.
"""
import argparse
import gzip
import json
import re
import sys
import time
import urllib.request

from pathlib import Path

WANT = {"rotationMode": 0, "messageDwellSec": 4, "scrollSpeed": 2, "debugTimeOffset": 0}
# The known networks and their order: as in Data/config.json (synced from the
# clock, never committed with values - no network names in this file).
_local = json.loads((Path(__file__).resolve().parent.parent / "Data" / "config.json").read_text(encoding="utf-8"))
WANT.update({k: _local[k] for k in ("wifiSSID", "wifiSSID2", "wifiSSID3") if k in _local})


def get(base, path):
    r = urllib.request.urlopen(urllib.request.Request(base + path, headers={"Accept-Encoding": "gzip"}), timeout=30)
    b = r.read()
    return gzip.decompress(b) if r.headers.get("Content-Encoding") == "gzip" else b


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", default="http://jewishclock.local")
    ap.add_argument("--version")
    a = ap.parse_args()
    for _ in range(60):   # until it has its time
        try:
            st = json.loads(get(a.clock, "/api/status"))
            if st.get("synced"):
                break
        except Exception:
            pass
        time.sleep(3)
    checks = []
    def check(name, ok, detail):
        checks.append(ok)
        print("%s %-28s %s" % ("OK  " if ok else "FAIL", name, detail))
    check("firmware", not a.version or st.get("firmware") == a.version, st.get("firmware"))
    check("time synced", bool(st.get("synced")), "utc %s" % st.get("utcTime"))
    check("update state", st.get("updateState") == "ok", st.get("updateState"))
    cfg = json.loads(get(a.clock, "/api/backup"))["config"]
    for k, v in WANT.items():
        check(k, cfg.get(k) == v, cfg.get(k))
    ev = json.loads(get(a.clock, "/api/events/list"))
    check("events", len(ev) > 0, "%d events" % len(ev))
    check("no test events", not [e for e in ev if e["name"].startswith("AP test") or "ניסוי" in e["name"]], "")
    imgs = json.loads(get(a.clock, "/api/images/list"))
    used = {e.get("image") for e in ev if e.get("image")}
    check("event pictures there", used <= {i["id"] for i in imgs}, "%d pictures, %d used" % (len(imgs), len(used)))
    for doc in ("/start.html", "/manual.html"):
        stamps = re.findall(r'class="fwver">([^<]*)<', get(a.clock, doc).decode("utf-8"))
        check(doc + " stamped", stamps and all(s == st.get("firmware") for s in stamps), stamps)
    page = get(a.clock, "/").decode("utf-8")
    check("page links", all(h in page for h in ('href="/start.html"', 'href="/manual.html"', 'href="/licenses.txt"')), "")
    print("== %d of %d OK ==" % (sum(checks), len(checks)))
    sys.exit(0 if all(checks) else 1)


if __name__ == "__main__":
    main()
