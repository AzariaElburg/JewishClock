"""
The full simulator's web server against the real clock's recorded answers
(sim/record_clock_web.py -> reference/clock-<date>/web/index.json + bodies/).

    C:/pio3/jc_clean/venv/Scripts/python sim/compare_web.py [--sim URL (default: the trips' clock, sim_ports.py)] [--ref DIR]

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


def current_page_files():
    """What this version's page files are on the clock (utils/web_files.py - gzip with mtime 0,
    so exactly reproducible): a page file may differ from an older recording only if it's this."""
    sys.path.insert(0, str(ROOT / "utils"))
    import web_files
    out = {}
    for rel, data, _src in web_files.clock_files(ROOT / "Data"):
        p = "/" + rel[:-3] if rel.endswith(".gz") else "/" + rel
        out[p] = data
    out["/"] = out.get("/index.html")
    out["/restore"] = out.get("/index.html")
    out["/no/such/page"] = out.get("/index.html")
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from sim_ports import urls
    ap.add_argument("--sim", default=urls("trips")[0])
    # The newest recording - a reference folder may hold other things only (e.g. a boot log).
    ap.add_argument("--ref", default=str(sorted(p.parent for p in (ROOT / "reference").glob("clock-*/web/index.json"))[-1]))
    args = ap.parse_args()
    ref = Path(args.ref)
    rec = json.loads((ref / "index.json").read_text(encoding="utf-8"))
    posts = dict(__import__("record_clock_web").POSTS)
    current = current_page_files()
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
        # A page file changed on purpose since the recording: exactly this version's file -> fine
        # (and a recorded 304 is then a 200: the ETag changed with the file).
        if m["method"] == "GET" and m["path"] in current and status == 200 and body == current[m["path"]]:
            ok += 1
            continue
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
                    # A field added since the recording (a newer version than the
                    # recorded clock) is news, not a difference; a missing or changed
                    # one is (2026-11-08: logLevelKeep/Until failed the check).
                    added = [x for x in d if x.endswith(" only in the simulator")]
                    if added:
                        print("NEW  %s %s: %s" % (m["method"], m["path"], ", ".join(x.split(" ")[0] for x in added)))
                    problems += [x for x in d if x not in added][:8]
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
