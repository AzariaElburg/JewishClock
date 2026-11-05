"""
Records how the real clock's web server answers - the reference the
simulator's web server is checked against once the clock is gone.

    C:/pio3/jc_clean/venv/Scripts/python sim/record_clock_web.py [--clock HOST] [--out DIR]

Changes nothing on the clock: GETs of every read route (body + headers, with
and without gzip, and the 304 for a page file), requests the clock refuses
(missing body/id, unknown paths, the bare /api/events parent - the library's
prefix matching), and the page itself in a browser, clicked through every
view and settings section without saving (HAR: every request and reply).
Out: reference/clock-<date>/web/ (out of git, see .gitignore).
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GETS = ["/", "/update.html", "/sortable.min.js", "/licenses.txt", "/start.html", "/manual.html", "/restore",
        "/update-safe", "/api/status", "/api/config", "/api/calendar", "/api/preview", "/api/events/list",
        "/api/events/excel-template", "/api/images/list", "/api/builtin/list", "/api/wifi/networks",
        "/api/backup", "/api/debug/diagnostic-log", "/api/debug/flash", "/api/debug/screenshot",
        "/api/location/search-result",
        # refused / odd ones
        "/api/events", "/api/events/blahblah", "/no/such/page", "/api/images/get?id=nosuchid",
        "/api/builtin/get?which=custom&name=nosuch"]
POSTS = [("/api/events/upsert", None), ("/api/events/delete", b""), ("/api/events", b""),
         ("/api/config", None), ("/api/calendar/convert", b'body={"from":"gregorian","d":5,"m":11,"y":2026}'),
         ("/api/debug/time", None), ("/api/restore", None)]


def fetch(base, path, data=None, headers=None):
    req = urllib.request.Request(base + path, data=data, headers=headers or {})
    if data is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            status, hdrs, body = r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        status, hdrs, body = e.code, dict(e.headers), e.read()
    except Exception as e:
        return {"path": path, "error": str(e)}, b""
    return {"path": path, "method": "POST" if data is not None else "GET", "status": status,
            "headers": hdrs, "bytes": len(body), "ms": round((time.time() - t0) * 1000)}, body


def safe_name(path):
    return path.strip("/").replace("/", "_").replace("?", "__").replace("&", "_").replace("=", "-") or "root"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", default="http://jewishclock.local")
    ap.add_argument("--out", default=str(ROOT / "reference" / ("clock-" + time.strftime("%Y-%m-%d")) / "web"))
    args = ap.parse_args()
    out = Path(args.out); (out / "bodies").mkdir(parents=True, exist_ok=True)
    index = []
    for path in GETS:
        for enc in ("identity", "gzip"):
            meta, body = fetch(args.clock, path, headers={"Accept-Encoding": enc})
            meta["accept"] = enc
            if body:
                (out / "bodies" / ("GET_%s_%s.bin" % (safe_name(path), enc))).write_bytes(body)
            index.append(meta)
            etag = meta.get("headers", {}).get("ETag")
            if etag and enc == "gzip":   # the page files' quick 304 (WebUI.cpp sendWebFile)
                m2, _ = fetch(args.clock, path, headers={"Accept-Encoding": "gzip", "If-None-Match": etag})
                m2["accept"] = "gzip+If-None-Match"
                index.append(m2)
            print(meta.get("status", meta.get("error")), meta.get("ms"), enc, path, flush=True)
    for path, data in POSTS:
        meta, body = fetch(args.clock, path, data=data if data is not None else b"")
        (out / "bodies" / ("POST_%s.bin" % safe_name(path))).write_bytes(body)
        index.append(meta); print(meta.get("status", meta.get("error")), "POST", path, flush=True)
    (out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1), encoding="utf-8")

    # The page in a browser: every view and settings section, nothing saved.
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        b = p.chromium.launch(channel="msedge", headless=True)
        ctx = b.new_context(viewport={"width": 1280, "height": 900}, record_har_path=str(out / "page.har"),
                            record_har_content="embed")
        pg = ctx.new_page()
        errors = []
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.goto(args.clock + "/", wait_until="load")
        pg.wait_for_function("typeof events !== 'undefined' && events.length > 0", timeout=60000)
        pg.wait_for_timeout(1500)
        pg.screenshot(path=str(out / "page_calendar.png"), full_page=True)
        for view in pg.eval_on_selector_all("button.rail-item[data-view]", "bs => bs.map(b => b.dataset.view)"):
            pg.click("button.rail-item[data-view=%s]" % view)
            pg.wait_for_timeout(1500)
            pg.screenshot(path=str(out / ("page_%s.png" % view)), full_page=True)
        pg.reload(wait_until="load")      # a repeat visit: the 304s
        pg.wait_for_timeout(2000)
        ctx.close(); b.close()
    (out / "page_errors.txt").write_text("\n".join(errors) + "\n", encoding="utf-8")
    print("->", out, "page errors:", len(errors))


if __name__ == "__main__":
    main()
