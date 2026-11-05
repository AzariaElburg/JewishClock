"""
The full simulator's web server against the real clock's recorded answers
(sim/record_clock_web.py -> reference/clock-<date>/web/index.json + bodies/).

    C:/pio3/jc_clean/venv/Scripts/python sim/compare_web.py [--sim http://127.0.0.1:8080] [--ref DIR]

Each recorded request is sent to the simulator; compared: status, the header
names in their order (values only where they don't depend on the moment:
Content-Type, Content-Encoding, Content-Disposition, Cache-Control,
Connection), and the body - page files and texts byte for byte, JSON by its
keys and value types (the values are the moment's: time, uptime, signal...).
"""
import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXED_HEADERS = ("Content-Type", "Content-Encoding", "Content-Disposition", "Cache-Control", "Connection", "Accept-Ranges")
# Bodies that are the moment's, not the program's: compared by length class only.
MOMENT_BODIES = ("/api/debug/screenshot", "/api/debug/diagnostic-log", "/api/backup", "/api/events/list",
                 "/api/wifi/networks")   # "scanning" on the first ask, the list once a scan has run


def fetch(base, m):
    req =urllib.request.Request(base + m["path"], data=m.get("_data"), headers=m.get("_headers", {}))
    if m.get("_data") is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, list(r.headers.items()), r.read()
    except urllib.error.HTTPError as e:
        return e.code, list(e.headers.items()), e.read()


def shape(v):
    if isinstance(v, dict):
        return {k: shape(x) for k, x in v.items()}
    if isinstance(v, list):
        return [shape(v[0])] if v else []
    return type(v).__name__.replace("int", "number").replace("float", "number")


def diff_shape(a, b, path=""):
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a.keys() - b.keys():
            out.append("%s.%s only on the clock" % (path, k))
        for k in b.keys() - a.keys():
            out.append("%s.%s only in the simulator" % (path, k))
        for k in a.keys() & b.keys():
            out += diff_shape(a[k], b[k], path + "." + k)
    elif isinstance(a, list) and isinstance(b, list):
        if a and b:
            out += diff_shape(a[0], b[0], path + "[]")
    elif a != b and not ({a, b} <= {"number", "NoneType"}):
        out.append("%s: %s on the clock, %s in the simulator" % (path, a, b))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default="http://127.0.0.1:8080")
    ap.add_argument("--ref", default=str(sorted((ROOT / "reference").glob("clock-*"))[-1] / "web"))
    args = ap.parse_args()
    ref = Path(args.ref)
    rec = json.loads((ref / "index.json").read_text(encoding="utf-8"))
    posts = dict(__import__("record_clock_web").POSTS)
    ok = bad = 0
    for m in rec:
        if "error" in m:
            continue
        name = m["path"].strip("/").replace("/", "_").replace("?", "__").replace("&", "_").replace("=", "-") or "root"
        if m["method"] == "POST":
            m["_data"] = posts.get(m["path"]) or b""
            body_file = ref / "bodies" / ("POST_%s.bin" % name)
        else:
            acc = m.get("accept") or "identity"
            m["_headers"] = {"Accept-Encoding": "gzip" if acc.startswith("gzip") else "identity"}
            if acc == "gzip+If-None-Match":
                m["_headers"]["If-None-Match"] = m["headers"].get("ETag", "")
                body_file = None
            else:
                body_file = ref / "bodies" / ("GET_%s_%s.bin" % (name, acc))
        status, headers, body = fetch(args.sim, m)
        problems = []
        if status != m["status"]:
            problems.append("status %s, clock %s" % (status, m["status"]))
        clock_h = list(m["headers"].items())
        if [k for k, _ in headers] != [k for k, _ in clock_h]:
            problems.append("headers %s, clock %s" % ([k for k, _ in headers], [k for k, _ in clock_h]))
        hs, hc = dict(headers), dict(clock_h)
        for k in FIXED_HEADERS:
            if hs.get(k) != hc.get(k):
                problems.append("%s: %r, clock %r" % (k, hs.get(k), hc.get(k)))
        if body_file and body_file.exists() and not any(m["path"].startswith(p) for p in MOMENT_BODIES):
            want = body_file.read_bytes()
            ctype = hc.get("Content-Type", "")
            if "json" in ctype and status == 200:
                try:
                    d = diff_shape(shape(json.loads(want)), shape(json.loads(body)))
                    problems += d[:8]
                except ValueError as e:
                    problems.append("JSON: %s" % e)
            elif body != want:
                problems.append("body differs (%d bytes, clock %d)" % (len(body), len(want)))
        tag = "%s %s [%s]" % (m["method"], m["path"], m.get("accept"))
        if problems:
            bad += 1
            print("DIFF", tag)
            for p in problems:
                print("     ", p)
        else:
            ok += 1
    print("== %d same, %d different ==" % (ok, bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
