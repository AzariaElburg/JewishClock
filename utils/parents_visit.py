"""Everything worth taking from the parents' clock on a visit - one command, read only.

    C:/pio3/jc_clean/venv/Scripts/python utils/parents_visit.py              # at the parents', laptop next to the clock
    C:/pio3/jc_clean/venv/Scripts/python utils/parents_visit.py --clock http://127.0.0.1:8071 --no-trip   # dry run on a simulator

This PC has one Wi-Fi adapter: on the clock's own Wi-Fi it has no internet (Claude is cut off too), so
the whole visit runs by itself (utils/ap_trip.py): joins the clock's Wi-Fi, takes everything below,
comes back to the network it was on, even when something fails. Changes NOTHING on the clock - the
settings + events backup is taken at the start and at the end and compared (a difference = said loudly).

Out: reference/visit-<date>/ (out of git, OneDrive-backed - can't be made again without the clock):
  status_start.json, backup_start.json, diagnostic_log.txt, pictures/, export.xlsx (the page's own
  Export, Both), web/ (sim/record_clock_web.py: every read route, refusals, the page clicked through),
  status_series.jsonl (/api/status every 10 s for --minutes: memory, screen hitches, signal),
  burst.json (several requests at once, as a browser sends them), odd.json (odd but harmless
  addresses), backup_end.json, SUMMARY.txt.
Left out on purpose (the user's clock): anything that saves, oversized requests, a Wi-Fi scan.
"""
import argparse
import concurrent.futures
import datetime
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import ap_trip  # noqa: E402

PY = r"C:/pio3/jc_clean/venv/Scripts/python"
ODD = ["/api/status?x=1", "/api/status?%41=%42", "/api/events/list?", "/%41pi/status", "/api//status",
       "/api/status/", "/index.html", "/API/STATUS", "/api/images/get", "/api/images/get?id=",
       "/api/builtin/get?which=builtin&name=", "/api/calendar?day=999"]


def get(base, path, timeout=60, headers=None):
    t0 = time.time()
    req = urllib.request.Request(base + path, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read(), round((time.time() - t0) * 1000)
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read(), round((time.time() - t0) * 1000)
    except Exception as e:
        return None, {}, str(e).encode(), round((time.time() - t0) * 1000)


def log(out, msg):
    ap_trip.log(msg)
    with open(out / "SUMMARY.txt", "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S ") + msg + "\n")


def step(out, name, fn):
    """One part; a failure is written down and the visit goes on."""
    try:
        fn()
        log(out, "OK      " + name)
        return True
    except Exception as e:
        log(out, "FAILED  %s: %s" % (name, e))
        return False


def backup(base):
    st, _, body, _ = get(base, "/api/backup")
    if st != 200:
        raise RuntimeError("backup: status %s" % st)
    j = json.loads(body)
    if "wifiSSID" not in json.dumps(j.get("config", {})):
        raise RuntimeError("backup looks wrong (no wifiSSID)")
    return j


def export_excel(base, dest):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        b = p.chromium.launch(channel="msedge", headless=True)
        ctx = b.new_context(accept_downloads=True)
        pg = ctx.new_page()
        pg.goto(base + "/", wait_until="load")
        pg.wait_for_function("typeof events !== 'undefined' && events.length > 0", timeout=60000)
        pg.click("button.rail-item[data-view=settings]")
        pg.click("#scope-both")
        with pg.expect_download(timeout=60000) as d:
            pg.click("button[onclick='downloadData()']")
        d.value.save_as(str(dest))
        ctx.close(); b.close()
    if dest.stat().st_size < 2000:
        raise RuntimeError("export only %d bytes" % dest.stat().st_size)


def visit(base, out, minutes):
    out.mkdir(parents=True, exist_ok=True)
    log(out, "clock %s, out %s" % (base, out))
    start = {}

    def status_start():
        st, _, body, ms = get(base, "/api/status")
        (out / "status_start.json").write_bytes(body)
        s = json.loads(body)
        log(out, "        version %s, up %.1f days, its Wi-Fi %s, heap %s (lowest %s), screen hitches %s" % (
            s.get("firmware"), (s.get("uptimeMs") or 0) / 86400000, s.get("apSsid"), s.get("freeHeap"),
            s.get("minFreeHeapEver"), s.get("displayHitches")))
    step(out, "status", status_start)

    def b_start():
        start["b"] = backup(base)
        (out / "backup_start.json").write_text(json.dumps(start["b"], ensure_ascii=False), encoding="utf-8")
    step(out, "backup (settings + events)", b_start)

    def diag():
        st, _, body, _ = get(base, "/api/debug/diagnostic-log", timeout=120)
        if st != 200 or len(body) < 100:
            raise RuntimeError("status %s, %d bytes" % (st, len(body)))
        (out / "diagnostic_log.txt").write_bytes(body)
        log(out, "        log %d KB" % (len(body) // 1024))
    step(out, "the clock's log", diag)

    def pictures():
        d = out / "pictures"; d.mkdir(exist_ok=True)
        st, _, body, _ = get(base, "/api/images/list")
        lst = json.loads(body) if st == 200 else []
        lst = lst.get("images", lst) if isinstance(lst, dict) else lst
        n = 0
        for it in lst:
            pid = it.get("id") if isinstance(it, dict) else it
            st, _, b, _ = get(base, "/api/images/get?id=%s" % pid)
            if st == 200:
                (d / ("%s.bmp" % pid)).write_bytes(b); n += 1
        st, _, body, _ = get(base, "/api/builtin/list")
        if st == 200:
            for it in json.loads(body).get("custom", []):
                name = it.get("name") if isinstance(it, dict) else it
                st, _, b, _ = get(base, "/api/builtin/get?which=custom&name=%s" % name)
                if st == 200:
                    (d / ("custom_%s.bmp" % name)).write_bytes(b); n += 1
        log(out, "        %d pictures/icons" % n)
    step(out, "event pictures and replaced icons", pictures)

    step(out, "Excel export (the page's Export, Both)", lambda: export_excel(base, out / "export.xlsx"))

    def record():
        r = subprocess.run([PY, str(ROOT / "sim" / "record_clock_web.py"), "--clock", base, "--out", str(out / "web")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1800)
        (out / "web_record_output.txt").write_text(r.stdout + r.stderr, encoding="utf-8")
        if r.returncode:
            raise RuntimeError("record_clock_web exit %d (web_record_output.txt)" % r.returncode)
    step(out, "web answers (record_clock_web)", record)

    def burst():
        res = []
        paths = ["/api/status", "/api/config", "/api/calendar", "/api/events/list", "/api/preview", "/api/status"]
        for round_ in range(3):
            with concurrent.futures.ThreadPoolExecutor(len(paths)) as ex:
                for p, (st, h, b, ms) in zip(paths, ex.map(lambda p: get(base, p), paths)):
                    res.append({"round": round_, "path": p, "status": st, "ms": ms, "bytes": len(b),
                                "connection": h.get("Connection")})
            time.sleep(3)
        (out / "burst.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
        log(out, "        %d of %d answered 200" % (sum(r["status"] == 200 for r in res), len(res)))
    step(out, "a burst of requests at once", burst)

    def odd():
        res = []
        for p in ODD:
            st, h, b, ms = get(base, p)
            res.append({"path": p, "status": st, "ms": ms, "bytes": len(b), "location": h.get("Location"),
                        "type": h.get("Content-Type"), "start": b[:120].decode("utf-8", "replace")})
        (out / "odd.json").write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")
    step(out, "odd but harmless addresses", odd)

    def series():
        end = time.time() + minutes * 60
        with open(out / "status_series.jsonl", "w", encoding="utf-8") as f:
            while time.time() < end:
                st, _, body, ms = get(base, "/api/status")
                f.write(json.dumps({"t": time.strftime("%H:%M:%S"), "ms": ms, "status": st,
                                    "s": json.loads(body) if st == 200 else None}) + "\n")
                f.flush()
                time.sleep(10)
    step(out, "/api/status every 10 s for %d min" % minutes, series)

    def b_end():
        end = backup(base)
        (out / "backup_end.json").write_text(json.dumps(end, ensure_ascii=False), encoding="utf-8")
        if "b" in start and end != start["b"]:
            raise RuntimeError("SETTINGS OR EVENTS DIFFER from the start - see backup_start/backup_end.json")
    step(out, "nothing changed on the clock (backup at the end = at the start)", b_end)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", default=ap_trip.CLOCK_URL)
    ap.add_argument("--clock-wifi", default=ap_trip.CLOCK_AP, help="the clock's own Wi-Fi name")
    ap.add_argument("--no-trip", action="store_true", help="the clock is reachable as it is (a simulator)")
    ap.add_argument("--minutes", type=int, default=5)
    ap.add_argument("--out", default=str(ROOT / "reference" / ("visit-" + datetime.date.today().isoformat())))
    a = ap.parse_args()
    out = Path(a.out)
    if a.no_trip:
        visit(a.clock, out, a.minutes)
    else:
        with ap_trip.on_clock_ap(a.clock_wifi) as url:
            visit(url, out, a.minutes)
    print("\n" + (out / "SUMMARY.txt").read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
