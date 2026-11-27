"""
Live view of the simulated clock in the browser: the screen as it moves,
for any date and time, at any speed.

    python sim/viewer.py            then open http://localhost:8765

Pick a date/time and speed and press Start: the simulator (C:/pio3/jc_clean/sim/build/jcsim.exe,
--live mode) restarts at that moment, with the file system from
sim/prepare_fs.py. The page shows the screen ~20 times a second and the
slide's text underneath.
"""
import http.server
import subprocess
import sys
import threading
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
WORK = SIM_WORK
EXE = SIM_EXE
LIVE = WORK / "live"
PORT = 8765

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>JewishClock simulator</title>
<style>
 body{font-family:system-ui,sans-serif;background:#1d1d1f;color:#eee;margin:24px}
 #screen{width:960px;height:256px;image-rendering:pixelated;background:#000;border:8px solid #333;border-radius:6px;display:block}
 .row{margin:14px 0;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
 input,select,button{font-size:15px;padding:5px 8px}
 #text{font-size:18px;direction:rtl;min-height:1.4em}
 #now{color:#aaa}
</style></head><body>
<h2>JewishClock simulator</h2>
<img id="screen" alt="clock screen">
<div class="row"><span id="now"></span></div>
<div class="row" id="text"></div>
<div class="row">
 <label>Date and time (Israel) <input id="when" type="datetime-local" step="1"></label>
 <label>Speed <select id="speed"><option>1</option><option>2</option><option>5</option><option>10</option><option>60</option></select>x</label>
 <button id="start">Start</button> <span id="msg"></span>
</div>
<script>
const $ = id => document.getElementById(id);
const pad = n => String(n).padStart(2, '0');
const d = new Date();
$('when').value = d.getFullYear()+'-'+pad(d.getMonth()+1)+'-'+pad(d.getDate())+'T'+pad(d.getHours())+':'+pad(d.getMinutes());
$('start').onclick = async () => {
  $('msg').textContent = 'starting...';
  const r = await fetch('/start?when=' + encodeURIComponent($('when').value.replace('T', ' ')) + '&speed=' + $('speed').value, {method: 'POST'});
  $('msg').textContent = await r.text();
};
async function tick() {
  try {
    const r = await fetch('/frame?' + Date.now());
    if (r.ok) { const b = await r.blob(); const u = URL.createObjectURL(b); const old = $('screen').src; $('screen').src = u; if (old.startsWith('blob:')) URL.revokeObjectURL(old); }
    const t = await fetch('/text?' + Date.now());
    if (t.ok) { const [now, text] = (await t.text()).split('\\n'); $('now').textContent = now || ''; $('text').textContent = text || ''; }
  } catch (e) {}
  setTimeout(tick, 50);
}
tick();
</script></body></html>
"""

proc = None
lock = threading.Lock()


def start_sim(when, speed):
    global proc
    with lock:
        if proc and proc.poll() is None:
            proc.kill(); proc.wait()
        LIVE.mkdir(parents=True, exist_ok=True)
        cmd = [str(EXE), "--fs", str(WORK / "fs"), "--out", str(LIVE), "--http-cache", str(WORK / "http_cache"),
               "--live", "--speed", str(speed)]
        if when:
            cmd += ["--time", when]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            return self.send(200, PAGE.encode("utf-8"), "text/html; charset=utf-8")
        f = {"/frame": LIVE / "latest.bmp", "/text": LIVE / "latest.txt"}.get(path)
        if f:
            try:
                return self.send(200, f.read_bytes(), "image/bmp" if path == "/frame" else "text/plain; charset=utf-8")
            except OSError:
                return self.send(404, b"", "text/plain")
        self.send(404, b"not found", "text/plain")

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if u.path != "/start":
            return self.send(404, b"not found", "text/plain")
        q = urllib.parse.parse_qs(u.query)
        when = q.get("when", [""])[0].strip()
        if len(when) == 16:
            when += ":00"
        try:
            speed = float(q.get("speed", ["1"])[0])
        except ValueError:
            speed = 1
        start_sim(when, speed)
        self.send(200, ("running from %s at %gx" % (when or "now", speed)).encode("utf-8"), "text/plain; charset=utf-8")


def main():
    if not EXE.exists():
        sys.exit("build the simulator first: python sim/build.py")
    if not (WORK / "fs" / "config.json").exists():
        sys.exit("prepare its file system first: python sim/prepare_fs.py")
    start_sim("", 1)
    print("JewishClock simulator: http://localhost:%d  (Ctrl+C to stop)" % PORT)
    try:
        http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
    finally:
        if proc and proc.poll() is None:
            proc.kill()


if __name__ == "__main__":
    main()
