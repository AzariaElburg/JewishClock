"""Installs an update package on the parents' clock over its own Wi-Fi, as they would, and checks that
nothing was lost (2026-11-27, the 2.2.0 visit). One trip (utils/ap_trip.py: this PC is cut off from the
internet meanwhile, and always comes back):

    C:/pio3/jc_clean/venv/Scripts/python utils/parents_install.py --package FILE.jcupdate --version 2.2.0 [--visit-first]

--visit-first: utils/parents_visit.py's read-only take first (backup, Excel export, log, pictures, web
answers) - and nothing installed unless every part of it went right.
Then: backup before, the update page's own install (utils/ap_tests.trip_update: the page, the restart, the
trial), the clock on the new version with its page files, backup after - settings and events compared key by
key (an update may add keys; none may change or go), the clock's own Wi-Fi password in its settings file.
Out: reference/visit-<date>/install-<version>/ (out of git, OneDrive-backed), SUMMARY.txt there.
"""
import argparse
import datetime
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import ap_trip     # noqa: E402
import ap_tests    # noqa: E402
import parents_visit   # noqa: E402

# Keys an update may set by itself (each why): anything else that changes is a loss.
MAY_CHANGE = {
    "apPassword": "2.2.0 copies the compiled-in password into the settings file (CLAUDE.md)",
    "updateKeyBy": "the update key's version: the running firmware's",
}


def kept(old, new):
    """Everything in `old` still in `new`: a list may grow (2.2.0: one time server -> three, the first
    kept), a dict may gain keys (2.2.0: places' countryCode); nothing may change or go."""
    if isinstance(old, dict):
        return isinstance(new, dict) and all(k in new and kept(v, new[k]) for k, v in old.items())
    if isinstance(old, list):
        return isinstance(new, list) and len(new) >= len(old) and all(kept(a, b) for a, b in zip(old, new))
    return old == new


def jget(base, path):
    with urllib.request.urlopen(base + path, timeout=30) as r:
        return json.loads(r.read())


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", required=True)
    ap.add_argument("--version", required=True, help="the version the package installs")
    ap.add_argument("--visit-first", action="store_true")
    ap.add_argument("--sim", nargs=2, type=int, metavar=("HOME_PORT", "AP_PORT"), help="a dry run on a simulator")
    a = ap.parse_args()
    trip = ap_trip.on_clock_ap
    if a.sim:
        ap_tests.use_simulator(*a.sim)
        trip = ap_tests.on_clock_ap
        ap_trip._connected_ssid = ap_tests._connected_ssid
        ap_trip._join = ap_tests._join
    package = Path(a.package).resolve()
    day = ROOT / "reference" / (("simvisit-" if a.sim else "visit-") + datetime.date.today().isoformat())
    out = day / ("install-" + a.version)
    out.mkdir(parents=True, exist_ok=True)
    ap_tests.OUT = out
    summary = out / "SUMMARY.txt"

    def say(ok, what, detail=""):
        line = "%s %-8s %s%s" % (time.strftime("%H:%M:%S"), "OK" if ok else "FAILED", what, ("  - " + detail) if detail else "")
        ap_trip.log(line)
        with open(summary, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        return ok

    with trip() as base:
        if a.visit_first:
            parents_visit.visit(base, day, 5)
            text = (day / "SUMMARY.txt").read_text(encoding="utf-8")
            if not say("FAILED" not in text, "read-only visit (reference/%s/SUMMARY.txt)" % day.name):
                say(False, "NOTHING INSTALLED - the visit had a failure")
                return 1
        st0 = jget(base, "/api/status")
        b0 = parents_visit.backup(base)
        (out / "backup_before.json").write_text(json.dumps(b0, ensure_ascii=False), encoding="utf-8")
        say(True, "before: version %s, %d events" % (st0.get("firmware"), len(b0.get("events", []))))

        from playwright.sync_api import sync_playwright
        with sync_playwright() as pw:
            br = pw.chromium.launch(channel="msedge")
            ap_tests.trip_update(base, br, package)
            br.close()
        if ap_trip._connected_ssid() != ap_trip.CLOCK_AP:
            ap_trip._join(ap_trip.CLOCK_AP, lambda: ap_trip._reachable(base + "/api/status"), 120)

        # the new version running, its trial over and its page files in (they go in after the trial)
        t0, st = time.time(), {}
        while time.time() - t0 < 600:
            try:
                st = jget(base, "/api/status")
                if st.get("firmware") == a.version and st.get("updateState", "ok") == "ok":
                    break
            except Exception:
                pass
            time.sleep(5)
        say(st.get("firmware") == a.version and st.get("updateState", "ok") == "ok", "the clock runs %s, trial over" % a.version,
            "status: %s, %s" % (st.get("firmware"), st.get("updateState")))
        time.sleep(30)
        page = urllib.request.urlopen(base + "/", timeout=30).read()
        say(len(page) > 1000, "its page opens", "%d bytes" % len(page))

        b1 = parents_visit.backup(base)
        (out / "backup_after.json").write_text(json.dumps(b1, ensure_ascii=False), encoding="utf-8")
        c0, c1 = b0.get("config", {}), b1.get("config", {})
        changed = sorted(k for k in c0 if k not in MAY_CHANGE and not kept(c0[k], c1.get(k)))
        grown = sorted(k for k in c0 if k not in MAY_CHANGE and kept(c0[k], c1.get(k)) and c1.get(k) != c0[k])
        say(not changed, "settings kept", ("changed or gone: %s" % changed) if changed else
            "new keys: %s; grown (old values kept): %s" % (sorted(set(c1) - set(c0)), grown))
        say(b1.get("events") == b0.get("events"), "events kept", "%d events" % len(b1.get("events", [])))
        used = jget(base, "/api/config").get("apPassword", "")
        say(bool(c1.get("apPassword")) and c1.get("apPassword") == used,
            "the clock's own Wi-Fi password is in its settings file (and is the one it uses)")
    print("\n" + summary.read_text(encoding="utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
