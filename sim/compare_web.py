"""
The full simulator's web server against the real clock's recorded answers
(sim/record_clock_web.py -> reference/clock-<date>/web/index.json + bodies/).

    C:/pio3/jc_clean/venv/Scripts/python sim/compare_web.py [--sim URL (default: the trips' clock, sim_ports.py)] [--ref DIR]

Each recorded request is sent to the simulator; compared: status, the header
names in their order (values only where they don't depend on the moment:
Content-Type, Content-Encoding, Content-Disposition, Cache-Control,
Connection), and the body - page files and texts byte for byte, JSON by its
keys and value types (the values are the moment's: time, uptime, signal...).

A recording made by the page (Advanced > Web answers > Record, imported with
sim/import_answers.py - "via": "browser"): a browser always asks for gzip, gives the
body unpacked and the header names as a set, without their order - compared so.
"""
import argparse
import gzip
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
# Bodies the firmware changed on purpose since the recording (path -> why): status and headers still
# compared, the body not - until the next recording at the parents' (CLAUDE.md) clears this list.
CHANGED_SINCE_RECORDING = {
    "/update-safe": "2026-11-18, 7148515: the built-in update page takes .jcupdate files only",
}
# Paths where the whole response changed on purpose (status + headers + body): skip entirely.
# A bare API path with no registered handler: 2.1.2 returned 404, current returns 200+page
# (SPA fallback - onNotFound serves index.html so the frontend router handles unknown paths).
SKIP_SINCE_RECORDING = {
    "/api/events":          "since 2.1.2: bare path with no handler → SPA fallback (200+page)",
    "/api/events/blahblah": "since 2.1.2: same (SPA fallback for any unregistered path)",
}


def fetch(base, m):
    req =urllib.request.Request(base + m["path"], data=m.get("_data"), headers=m.get("_headers", {}))
    if m.get("_data") is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, list(r.headers.items()), r.read()
    except urllib.error.HTTPError as e:
        return e.code, list(e.headers.items()), e.read()


# Headers a browser doesn't show a page (hop by hop) - left out of a browser recording's comparison
BROWSER_HIDDEN = {"connection", "keep-alive", "transfer-encoding"}


def unpacked(headers, body):
    """The body as a browser hands it to a page: gzip undone."""
    if dict((k.lower(), v) for k, v in headers).get("content-encoding") == "gzip" and body[:2] == b"\x1f\x8b":
        return gzip.decompress(body)
    return body


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


def current_page_files(staged_fs=None):
    """What the served page files are: from the already-staged filesystem when given (run_all_tests.py
    passes --staged-fs so we compare against exactly what was staged, not a re-generated copy that
    could differ if Data/ changed after staging), else re-generated from utils/web_files.py."""
    out = {}
    if staged_fs:
        staged = Path(staged_fs)
        for f in staged.rglob("*"):
            if not f.is_file():
                continue
            rel = f.relative_to(staged).as_posix()
            path = "/" + rel[:-3] if rel.endswith(".gz") else "/" + rel
            out[path] = f.read_bytes()
    else:
        sys.path.insert(0, str(ROOT / "utils"))
        import web_files
        for rel, data, _src in web_files.clock_files(ROOT / "Data"):
            p = "/" + rel[:-3] if rel.endswith(".gz") else "/" + rel
            out[p] = data
    out["/"] = out.get("/index.html")
    out["/restore"] = out.get("/index.html")
    out["/no/such/page"] = out.get("/index.html")
    # The Excel template is a Data/ file served under an API name (WebUI.cpp; it changes too - 2026-11-10: Icon column)
    out["/api/events/excel-template"] = out.get("/tools/JewishClock_Events_Template.xlsx")
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from sim_ports import urls
    ap.add_argument("--sim", default=urls("simulator_validity")[0])
    # The newest recording - a reference folder may hold other things only (e.g. a boot log).
    ap.add_argument("--ref", default=str(sorted(p.parent for p in (ROOT / "reference").glob("*/web/index.json"))[-1]))
    # Already-staged filesystem directory: compare page files against what was actually staged, not a
    # fresh re-generation from Data/ that may differ when Data/ changes after staging (run_all_tests.py).
    ap.add_argument("--staged-fs", default=None)
    args = ap.parse_args()
    ref = Path(args.ref)
    rec = json.loads((ref / "index.json").read_text(encoding="utf-8"))
    posts = dict(__import__("record_clock_web").POSTS)
    current = current_page_files(args.staged_fs)
    ok = bad = 0
    for m in rec:
        if "error" in m:
            continue
        name = m["path"].strip("/").replace("/", "_").replace("?", "__").replace("&", "_").replace("=", "-") or "root"
        browser = m.get("via") == "browser"
        if m["method"] == "POST":
            m["_data"] = posts.get(m["path"]) or b""
            body_file = ref / "bodies" / ("POST_%s.bin" % name)
        elif browser:
            m["_headers"] = {"Accept-Encoding": "gzip, deflate"}
            body_file = ref / "bodies" / ("GET_%s_browser.bin" % name)
            if m.get("accept") == "browser+If-None-Match":
                m["_headers"]["If-None-Match"] = m["headers"].get("etag", "")
                body_file = None
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
        if m["path"] in SKIP_SINCE_RECORDING:
            print("      (whole response changed on purpose: %s)" % SKIP_SINCE_RECORDING[m["path"]])
            ok += 1
            continue
        if browser:   # as the page saw it: unpacked, names in lower case
            body = unpacked(headers, body)
            headers = [(k.lower(), v) for k, v in headers]
        # A page file changed on purpose since the recording: exactly this version's file -> fine
        # (and a recorded 304 is then a 200: the ETag changed with the file).
        cur = current.get(m["path"])
        if browser and cur:
            cur = unpacked([("Content-Encoding", "gzip")], cur)
        if m["method"] == "GET" and cur is not None and status == 200 and body == cur:
            ok += 1
            continue
        if status != m["status"]:
            problems.append("status %s, clock %s" % (status, m["status"]))
        if browser:   # names as a set (a browser gives no order), lower case
            clock_names = set(m["headers"]) - BROWSER_HIDDEN
            names = {k for k, _ in headers} - BROWSER_HIDDEN
            added_h = sorted(k for k in names - clock_names if k.startswith("x-"))   # ours, added since: news
            if added_h:
                print("NEW  %s %s: header %s" % (m["method"], m["path"], ", ".join(added_h)))
                names -= set(added_h)
            if names != clock_names:
                problems.append("headers %s, clock %s" % (sorted(names), sorted(clock_names)))
            hs, hc = dict(headers), m["headers"]
            fixed = [h.lower() for h in FIXED_HEADERS if h.lower() not in BROWSER_HIDDEN]
        else:
            clock_h = list(m["headers"].items())
            # Our own X- header added since the recording: news, like an added JSON field
            # (2026-11-13: X-Screen-Moving). The library's headers stay compared in order.
            added_h = [k for k, _ in headers if k.startswith("X-") and k not in dict(clock_h)]
            if added_h:
                print("NEW  %s %s: header %s" % (m["method"], m["path"], ", ".join(added_h)))
                headers = [(k, v) for k, v in headers if k not in added_h]
            if [k for k, _ in headers] != [k for k, _ in clock_h]:
                problems.append("headers %s, clock %s" % ([k for k, _ in headers], [k for k, _ in clock_h]))
            hs, hc = dict(headers), dict(clock_h)
            fixed = FIXED_HEADERS
        for k in fixed:
            if hs.get(k) != hc.get(k):
                problems.append("%s: %r, clock %r" % (k, hs.get(k), hc.get(k)))
        if body_file and body_file.exists() and m["path"] in CHANGED_SINCE_RECORDING:
            print("      (body not compared - changed on purpose: %s)" % CHANGED_SINCE_RECORDING[m["path"]])
        elif body_file and body_file.exists() and not any(m["path"].startswith(p) for p in MOMENT_BODIES):
            want = body_file.read_bytes()
            ctype = {k.lower(): v for k, v in hc.items()}.get("content-type", "")
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
