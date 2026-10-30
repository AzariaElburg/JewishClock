"""Runs JavaScript in this folder's Data/index.html against the real clock and
takes a screenshot - for checking page changes before an uploadfs.

    python utils/page_test.py JS_FILE OUT_PNG [WIDTH,HEIGHT] [--clock ADDRESS]

Serves Data/index.html (plus a test hook) on localhost and relays every other
request to the clock, so the page is same-origin with the clock's API. JS_FILE
is the body of an async function; its return value goes to OUT_PNG + ".json"
(via headless Edge --dump-dom). The screenshot is OUT_PNG (an absolute path:
Edge resolves relative ones elsewhere). "no result in the page" only means
the hook didn't finish in time - the screenshot is still taken.
"""
import argparse
import http.server
import json
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.request
from pathlib import Path

PROJ = Path(__file__).resolve().parent.parent
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
PORT = 8792

ap = argparse.ArgumentParser()
ap.add_argument("js")
ap.add_argument("out")
ap.add_argument("size", nargs="?", default="1000,1300")
ap.add_argument("--clock", default="jewishclock.local")
args = ap.parse_args()
CLOCK = "http://" + args.clock
out_path = str(Path(args.out).resolve())

js = Path(args.js).read_text(encoding="utf-8")
hook = ("<script>window.addEventListener('load', () => setTimeout(async () => {\n"
        "  let out; try { out = await (async () => {\n" + js + "\n})(); } catch (e) { out = { error: String(e && e.stack || e) }; }\n"
        "  const p = document.createElement('pre'); p.id = 'out'; p.textContent = JSON.stringify(out); document.body.append(p);\n"
        "}, 1500));</script></body>")
page = (PROJ / "Data" / "index.html").read_text(encoding="utf-8").replace("</body>", hook).encode("utf-8")


class Relay(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def relay(self, method):
        if method == "GET" and self.path.split("?")[0] in ("/", "/index.html"):
            self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page))); self.end_headers(); self.wfile.write(page); return
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)) if method == "POST" else None
        req = urllib.request.Request(CLOCK + self.path, data=body, method=method)
        if self.headers.get("Content-Type"): req.add_header("Content-Type", self.headers["Content-Type"])
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data, code, ctype = r.read(), r.status, r.headers.get("Content-Type", "application/octet-stream")
        except urllib.error.HTTPError as e:
            data, code, ctype = e.read(), e.code, e.headers.get("Content-Type", "text/plain")
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def do_GET(self): self.relay("GET")
    def do_POST(self): self.relay("POST")


srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Relay)
threading.Thread(target=srv.serve_forever, daemon=True).start()
dom = subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--virtual-time-budget=120000", "--hide-scrollbars",
                      "--window-size=" + args.size, "--screenshot=" + out_path, "http://127.0.0.1:%d/" % PORT],
                     capture_output=True, timeout=300).stdout.decode("utf-8", "replace")
srv.shutdown()
m = re.search(r'<pre id="out">(.*?)</pre>', dom, re.S)
if not m:
    sys.exit("no result in the page")
txt = m.group(1).replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
Path(out_path + ".json").write_text(txt, encoding="utf-8")
res = json.loads(txt)
print(json.dumps({k: (v if not isinstance(v, str) or len(v) < 200 else v[:60] + "...(%d chars)" % len(v)) for k, v in res.items()}, ensure_ascii=False, indent=1))
