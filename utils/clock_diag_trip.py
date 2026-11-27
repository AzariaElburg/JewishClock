"""One read-only trip to the clock's own Wi-Fi (2026-11-20, at the parents': the clock was on
"JewishClock-Setup" - not on the home Wi-Fi). Takes what shows why, comes back to the network
the PC was on (utils/ap_trip.py). Changes nothing on the clock.

    C:/pio3/jc_clean/venv/Scripts/python utils/clock_diag_trip.py [--clock-wifi JewishClock-Setup]

Out: reference/visit-<date>/diag-<time>/ : status.json, config_wifi.json (names and places only -
never passwords), networks.json (what the clock hears), diagnostic_log.txt, SUMMARY.txt.
"""
import datetime
import json
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import ap_trip  # noqa: E402

OUT = ROOT / "reference" / ("visit-" + datetime.date.today().isoformat()) / ("diag-" + time.strftime("%H%M%S"))


def get(base, path, timeout=30):
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            return r.status, r.read()
    except Exception as e:
        return None, str(e).encode()


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    wifi = sys.argv[sys.argv.index("--clock-wifi") + 1] if "--clock-wifi" in sys.argv else "JewishClock-Setup"
    OUT.mkdir(parents=True, exist_ok=True)
    notes = []
    with ap_trip.on_clock_ap(wifi) as base:
        for name, path in (("status.json", "/api/status"), ("diagnostic_log.txt", "/api/debug/diagnostic-log")):
            st, body = get(base, path, 60)
            (OUT / name).write_bytes(body)
            notes.append("%s %s: %s, %d bytes" % (path, name, st, len(body)))
        st, body = get(base, "/api/config")
        try:
            c = json.loads(body)
            keep = {k: v for k, v in c.items() if ("ssid" in k.lower() or k in ("places", "wifiLastNetwork", "projectName", "apSsid"))
                    and "pass" not in k.lower()}
        except Exception as e:
            keep = {"error": str(e), "status": st}
        (OUT / "config_wifi.json").write_text(json.dumps(keep, ensure_ascii=False, indent=1), encoding="utf-8")
        notes.append("/api/config: %s (Wi-Fi names and places kept, no passwords)" % st)
        # the networks the clock hears (asks it to scan; the answer may say "scanning" first)
        nets = None
        for _ in range(10):
            st, body = get(base, "/api/wifi/networks", 30)
            try:
                nets = json.loads(body)
            except Exception:
                nets = {"raw": body.decode("utf-8", "replace")[:500]}
            if isinstance(nets, dict) and nets.get("networks"):
                break
            time.sleep(3)
        (OUT / "networks.json").write_text(json.dumps(nets, ensure_ascii=False, indent=1), encoding="utf-8")
        notes.append("/api/wifi/networks: %s" % st)
    (OUT / "SUMMARY.txt").write_text("\n".join(notes) + "\n", encoding="utf-8")
    print("\n".join(notes))
    print("->", OUT)


if __name__ == "__main__":
    main()
