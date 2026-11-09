"""
Every control of the clock's web pages, used in a browser the way a person
would, each result checked on the clock itself - not just "the click didn't
fail" (2026-11-07: the debug time's Hebrew Apply failed at the parents' and no
test had ever pressed it).

    C:/pio3/jc_clean/venv/Scripts/python sim/ui_tests.py [--only SECTION,...] [--keep] [--show] [--port-offset N]
    C:/pio3/jc_clean/venv/Scripts/python sim/ui_tests.py --list

Runs its own simulated clock (sim/build/jcclock.exe - build it first: sim/build_clock.py),
fresh from Data/ each run, on 127.0.0.1:8060 (its own Wi-Fi 8061, control 8069):
the tests factory-reset it, change its Wi-Fi, fill its bin - never the dev or
family simulators. Browser: Edge via Playwright.

COVERAGE is measured, not declared: a script in the page records every control
that is ever on screen - form fields (each option of a select, both states of
a switch), links, anything with a click handler, anything the mouse shows as
clickable or draggable (delegated ones: icon choices, drag handles, list rows) -
and every one a real click / typing / choice reached. A control seen but never
used fails the run ("UNCOVERED"). A new button needs a test, or the run fails.
KNOWN_UNTESTABLE lists the few that can't be driven here, each with its reason.

Results: C:/pio3/jc_clean/sim/ui_tests/<time>/summary.txt (+ a screenshot per failure).
"""
import datetime
import json
import os
import re
import shutil
import socket
import stat
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import web_files  # noqa: E402

EXE = Path("C:/pio3/jc_clean/sim/build/jcclock.exe")
OUT = Path("C:/pio3/jc_clean/sim/ui_tests") / time.strftime("%Y%m%d_%H%M%S")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_ports import ports, urls  # noqa: E402
PORT_HOME, PORT_AP, PORT_CTL = ports("ui")   # sim_ports.py
HOME, AP, CTL = ("http://127.0.0.1:%d" % p for p in (PORT_HOME, PORT_AP, PORT_CTL))
FILES = ROOT / "sim" / "ui_files"   # test inputs (pictures, a video, Excel files)

results = []


# ── plumbing ──────────────────────────────────────────────────────────────────
def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(OUT / "summary.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def check(name, ok, detail=""):
    results.append((CURRENT[0], name, bool(ok), str(detail)))
    log("%s [%s] %s %s" % ("PASS" if ok else "FAIL", CURRENT[0], name, str(detail)[:300]))
    if not ok and PAGE[0] is not None:
        try:
            PAGE[0].screenshot(path=str(OUT / ("fail_%03d.png" % len(results))))
        except Exception:
            pass
    return ok


CURRENT = ["-"]
PAGE = [None]


def http(base, path, data=None, form=None, timeout=20, raw=False):
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(base + path, data=data)
    if form is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body, code = r.read(), r.status
    except urllib.error.HTTPError as e:
        body, code = e.read(), e.code
    return (code, body) if raw else body


def jget(path, base=None):
    return json.loads(http(base or HOME, path))


def api_post(path, obj, base=None):
    return http(base or HOME, path, form={"body": json.dumps(obj, ensure_ascii=False)}, raw=True)


def cfg():
    return jget("/api/config")


def status():
    return jget("/api/status")


def events():
    j = jget("/api/events/list")
    return j if isinstance(j, list) else j.get("events", [])


def wait(cond, timeout=10, step=0.25):
    end = time.time() + timeout
    while True:
        try:
            v = cond()
            if v:
                return v
        except Exception:
            pass
        if time.time() > end:
            return None
        time.sleep(step)


def wait_cfg(key, value, timeout=8):
    """A live setting lands about a second after the last change (liveSave)."""
    got = wait(lambda: (lambda c: c if same(c.get(key), value) else None)(cfg()), timeout)
    return got is not None


def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        try:
            return abs(float(a) - float(b)) < 0.06
        except (TypeError, ValueError):
            return False
    return a == b


def ctl(path, body=None):
    try:
        return http(CTL, path, data=body if body is not None else (None if path in ("/state", "/world") else b""), timeout=10)
    except Exception:
        return b""


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        return True
    except OSError:
        return False


def up(base=None):
    try:
        return bool(http(base or HOME, "/api/status", timeout=3))
    except Exception:
        return False


def synced():
    try:
        return status().get("synced")
    except Exception:
        return False


def rmtree(p):
    def clear(func, path, _):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    if Path(p).exists():
        shutil.rmtree(p, onerror=clear)


class Clock:
    """The tests' own simulated clock: Data/ as uploadfs would put it."""
    def __init__(self):
        if listening(PORT_CTL) or listening(PORT_HOME):
            ctl("/stop")
            wait(lambda: not listening(PORT_CTL) and not listening(PORT_HOME), 15)
        self.dir = OUT / "clock"
        self.fs, self.state = self.dir / "fs", self.dir / "state"
        web_files.stage(ROOT / "Data", self.fs)
        for d in ("images", "custom"):
            if (ROOT / "Data" / d).is_dir():
                shutil.copytree(ROOT / "Data" / d, self.fs / d, dirs_exist_ok=True)
        self.state.mkdir(parents=True)
        shutil.copy2(EXE, self.dir / "jcclock.exe")   # its own copy: a running clock locks its program file (builds failed on it)
        self.launch()

    def launch(self):
        """Starts it (again, after stop(): its flash and settings as they were)."""
        exe = self.dir / "jcclock.exe"
        self.logf = open(self.dir / "run.log", "ab")
        self.p = subprocess.Popen([str(exe), "--fs", str(self.fs), "--state", str(self.state), "--verbose",
                                   "--home-port", str(PORT_HOME), "--ap-port", str(PORT_AP), "--control-port", str(PORT_CTL)],
                                  stdout=self.logf, stderr=subprocess.STDOUT, cwd=str(self.dir),
                                  env=dict(os.environ, JC_TITLE="Test (ui_tests)"))

    def stop(self):
        ctl("/stop")
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            pass
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.p.pid)], capture_output=True)
        self.logf.close()


# ── coverage: recorded inside the page ────────────────────────────────────────
# Keys: the element's id; else its onclick (numbers -> N: one key for every row
# of a kind); else the nearest ancestor id + tag + classes + data-* names (values
# only for the attributes in KEEP_VALUE, where each value is its own choice).
COVERAGE_JS = r"""
(() => {
  if (window.__cov) return;
  const cov = window.__cov = { seen: {}, used: {} };
  const LISTEN = new Set(['click', 'change', 'input', 'pointerdown', 'mousedown', 'dragstart', 'drop', 'keydown']);
  const origAdd = EventTarget.prototype.addEventListener;
  EventTarget.prototype.addEventListener = function (type, fn, opts) {
    if (LISTEN.has(type) && this instanceof Element && this !== document.body && this !== document.documentElement) this.__covListen = true;
    return origAdd.call(this, type, fn, opts);
  };
  for (const prop of ['onclick', 'onchange', 'oninput', 'onmousedown', 'onpointerdown']) {
    const d = Object.getOwnPropertyDescriptor(HTMLElement.prototype, prop);
    Object.defineProperty(HTMLElement.prototype, prop, { configurable: true, enumerable: true,
      get() { return d.get.call(this); },
      set(fn) { if (fn) this.__covListen = true; d.set.call(this, fn); } });
  }
  const KEEP_VALUE = new Set(['data-icon', 'data-view', 'data-type', 'data-cfg', 'data-which', 'data-name', 'data-a', 'data-fps']);
  const STATE_CLASSES = new Set(['active', 'open', 'show', 'hidden', 'sel', 'selected', 'disabled', 'dragging', 'flash',
    'field-missing', 'lit', 'weak', 'offline', 'today', 'has-ev', 'connected', 'busy', 'autofill-flash', 'search-miss', 'searching']);
  const LIST_ROWS = new Set(['place-hit', 'gal-item', 'search-item']);   // one key for every row of such a list
  const FIELD = /^(INPUT|SELECT|TEXTAREA)$/;
  const POINTERS = new Set(['pointer', 'grab', 'grabbing', 'move', 'col-resize', 'row-resize', 'ew-resize', 'ns-resize']);
  // Straight to the test (a page that reloads or navigates by itself loses window.__cov).
  function push(kind, k, info) { try { if (window.__covPush) window.__covPush(kind, k, info); } catch (e) {} }
  function visible(e) { return !!(e.offsetWidth || e.offsetHeight || e.getClientRects().length) && getComputedStyle(e).visibility !== 'hidden'; }
  function isControl(e) {
    if (!(e instanceof HTMLElement) || e === document.body) return false;
    if (FIELD.test(e.tagName)) return e.type !== 'hidden';
    if (e.tagName === 'A' && e.hasAttribute('href')) return true;
    if (e.tagName === 'BUTTON' || e.hasAttribute('onclick') || e.__covListen) return true;
    const c = getComputedStyle(e).cursor;
    if (!POINTERS.has(c)) return false;
    const p = e.parentElement;   // the outermost of a run with the same pointer (not a button's inner span)
    return !(p && getComputedStyle(p).cursor === c);
  }
  function ctrlAncestor(e) {   // inside another control (a span in a button, a label's text): that one counts
    for (let p = e.parentElement; p && p !== document.body; p = p.parentElement)
      if (p.tagName === 'BUTTON' || p.tagName === 'A' || p.tagName === 'LABEL') return p;
    return null;
  }
  function key(e) {
    if (e.id) return '#' + e.id;
    const oc = e.getAttribute('onclick');
    if (oc) return 'onclick:' + oc.replace(/\s+/g, ' ').replace(/\d+/g, 'N').slice(0, 90);
    const anc = e.parentElement && e.parentElement.closest('[id]');
    const data = [...e.attributes].filter(a => a.name.startsWith('data-'))
      .map(a => KEEP_VALUE.has(a.name) ? a.name + '=' + a.value : a.name);
    const cls = [...e.classList].filter(c => !STATE_CLASSES.has(c)).sort();
    let k = e.tagName.toLowerCase() + (cls.length ? '.' + cls.join('.') : '') + data.map(d => '[' + d + ']').join('');
    if (e.tagName === 'A') return k + '[href=' + e.getAttribute('href').replace(/\?.*/, '') + ']';
    if (e.tagName === 'BUTTON' && !data.length && !cls.some(c => LIST_ROWS.has(c))) k += ' "' + e.textContent.trim().replace(/\d+/g, 'N').slice(0, 30) + '"';
    else if (!data.length && !cls.length) k += ' "' + (e.getAttribute('title') || e.textContent || '').trim().replace(/\d+/g, 'N').slice(0, 30) + '"';
    return (anc ? '#' + anc.id + ' ' : '') + k;
  }
  function kinds(e) {   // every way of using it: a select's options, a switch's two states
    const k = key(e);
    if (e.tagName === 'SELECT') return [...e.options].filter(o => !o.disabled).map(o => k + '=' + o.value);
    if (e.type === 'checkbox' || e.type === 'radio') return [k + '=on', k + '=off'];
    return [k];
  }
  function where() { return location.pathname + (document.getElementById('restore-view') && !document.getElementById('restore-view').classList.contains('hidden') ? '#restore' : ''); }
  const ours = () => location.protocol === 'http:';   // not the browser's own "can't be reached" page
  cov.scan = () => {
    if (!ours()) return 0;
    const w = where();
    document.querySelectorAll('*').forEach(e => {
      if (!isControl(e) || ctrlAncestor(e) && !FIELD.test(e.tagName)) return;
      if (!(e.type === 'file') && !visible(e)) return;   // a file box is hidden behind its button
      if (e.disabled) return;
      for (const k of kinds(e)) if (!cov.seen[k]) {
        cov.seen[k] = { where: w, text: (e.textContent || e.placeholder || e.title || '').trim().slice(0, 40) };
        push('seen', k, cov.seen[k]);
      }
    });
    return Object.keys(cov.seen).length;
  };
  function used(e, how) {
    const k = key(e);
    let ks = [k];
    if (e.tagName === 'SELECT') ks = [k + '=' + e.value];
    else if (e.type === 'checkbox' || e.type === 'radio') ks = [k + '=' + (e.checked ? 'on' : 'off')];
    for (const x of ks) { cov.used[x] = (cov.used[x] || 0) + 1; push('used', x, null); }
  }
  const TEXTY = /^(text|number|search|password|date|time|datetime-local|range|email|url|tel|)$/;
  for (const type of ['click', 'pointerdown', 'input', 'change']) {
    origAdd.call(window, type, ev => {
      // A click must be a real one (the page clicks file boxes and download links itself);
      // input/change: the page never makes those itself, Playwright's choices are fine.
      if (!ev.isTrusted && (type === 'click' || type === 'pointerdown')) return;
      if (!ours()) return;
      const t = ev.target;
      if (type === 'input' || type === 'change') {
        if (t instanceof HTMLElement && FIELD.test(t.tagName)) {
          if (t.tagName === 'SELECT' || t.type === 'checkbox' || t.type === 'radio' || t.type === 'file' ? type === 'change' : TEXTY.test(t.type)) used(t, type);
        }
        return;
      }
      // click / pointerdown: every control on the way up (a row's button, then the row)
      for (const e of ev.composedPath()) {
        if (e === document.body || e === document || e === window) break;
        if (!(e instanceof HTMLElement)) continue;   // an SVG drawing inside a button
        if (FIELD.test(e.tagName) && e.tagName !== 'INPUT') continue;   // opening a select is not choosing
        if (e.tagName === 'INPUT' && e.type !== 'button' && e.type !== 'submit') continue;
        if (isControl(e) && !(ctrlAncestor(e) && !FIELD.test(e.tagName))) used(e, type);
      }
    }, true);
  }
  const scanSoon = () => { clearTimeout(cov.t); cov.t = setTimeout(cov.scan, 120); };
  origAdd.call(document, 'DOMContentLoaded', () => {
    cov.scan();
    new MutationObserver(scanSoon).observe(document.body, { subtree: true, childList: true, attributes: true, attributeFilter: ['class', 'style', 'hidden', 'disabled'] });
  });
})();
"""

SEEN, USED = {}, {}


def harvest(page):
    """Collect the page's coverage (before it navigates away / closes)."""
    try:
        page.evaluate("window.__cov && window.__cov.scan()")
        c = page.evaluate("window.__cov ? {seen: window.__cov.seen, used: window.__cov.used} : null")
    except Exception:
        return
    if not c:
        return
    for k, v in c["seen"].items():
        SEEN.setdefault(k, v)
    for k, v in c["used"].items():
        USED[k] = USED.get(k, 0) + v


# Controls the browser can't drive here, each with the reason. Keep it short.
KNOWN_UNTESTABLE = {
}


# ── the browser ───────────────────────────────────────────────────────────────
class UI:
    def __init__(self, pw, show=False):
        self.browser = pw.chromium.launch(channel="msedge", headless=not show)
        self.ctx = self.browser.new_context(viewport={"width": 1280, "height": 900}, accept_downloads=True)
        self.ctx.expose_function("__covPush", self._cov)
        self.ctx.add_init_script(COVERAGE_JS)
        # Messages stay next to what was clicked until a click elsewhere (2026-11-07). A person's
        # click on a control under one closes it; Playwright instead waits for the control to be
        # uncovered. So clicks pass through a message here - except where a test means to click
        # it (.test-hit: "a click closes a message") and its own buttons (Undo).
        self.ctx.add_init_script("""document.addEventListener('DOMContentLoaded', () => {
            const st = document.createElement('style');
            st.textContent = '#toast:not(.test-hit){pointer-events:none} #toast button{pointer-events:auto}';
            document.head.append(st); });""")
        self.dialogs = []          # native confirm()/alert(): (type, message)
        self.dialog_answer = [True]
        self.ctx.on("page", self._hook)
        self.page = self.ctx.new_page()
        PAGE[0] = self.page

    def _cov(self, kind, key, info):
        if kind == "seen":
            SEEN.setdefault(key, info)
        else:
            USED[key] = USED.get(key, 0) + 1

    def _hook(self, page):
        page.on("dialog", self._dialog)
        page.on("pageerror", lambda e: check("no script error on the page", False, str(e)[:300]))
        page.on("close", lambda p: None)

    def _dialog(self, d):
        self.dialogs.append((d.type, d.message))
        (d.accept if self.dialog_answer[0] else d.dismiss)()

    def goto(self, url, wait_ms=1500):
        harvest(self.page)
        self.page.goto(url, wait_until="load")
        self.page.wait_for_timeout(wait_ms)

    def settings(self):
        self.page.click("button.rail-item[data-view=settings]")
        self.page.wait_for_timeout(800)

    def calendar(self):
        self.page.click("button.rail-item[data-view=calendar]")
        self.page.wait_for_timeout(400)

    def toast(self):
        t = self.page.locator("#toast")
        return t.inner_text() if "show" in (t.get_attribute("class") or "") else ""

    def wait_toast(self, timeout=8):
        return wait(lambda: self.toast(), timeout, 0.1) or ""

    def clear_toast(self):
        self.page.evaluate("el('toast').classList.remove('show'); el('toast').textContent = ''")

    def click_msg(self, selector, timeout=8):
        """Click, and return the message it brings (an old one is cleared first)."""
        self.clear_toast()
        self.page.click(selector)
        return self.wait_toast(timeout)

    def ask_answer(self, label, timeout=5000):
        """Press a button in the page's own question box (askBox)."""
        self.page.locator("#cf-buttons button", has_text=label).first.click(timeout=timeout)
        self.page.wait_for_timeout(300)

    def close(self):
        harvest(self.page)
        for p in self.ctx.pages:
            harvest(p)
        self.browser.close()


# ── the sections ──────────────────────────────────────────────────────────────
SECTIONS = []


def section(fn):
    SECTIONS.append(fn)
    return fn


def screen():
    try:
        return json.loads(ctl("/state") or b"{}").get("screen", "")
    except ValueError:
        return ""


def screen_shows(pred, timeout=60):
    """Waits until the clock's screen shows a text pred() accepts; returns it (or None) - and what it showed."""
    seen = []
    end = time.time() + timeout
    while time.time() < end:
        t = screen()
        if t and (not seen or seen[-1] != t):
            seen.append(t)
        if t and pred(t):
            return t, seen
        time.sleep(0.3)
    return None, seen


def convert(**body):
    code, b = api_post("/api/calendar/convert", body)
    return json.loads(b)


def add_event(**kw):
    ev = {"name": "UIT", "hasG": False, "gD": 0, "gM": 0, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jY": "", "jYNum": 0,
          "isAdarII": False, "afterSunset": False, "type": "other", "icon": "", "warn": 0, "image": ""}
    ev.update(kw)
    code, b = api_post("/api/events/upsert", ev)
    return json.loads(b).get("id")


def store_events(*new):
    """Events the dialog would refuse (dates that don't match), stored the one way
    they can still get in: an import (it saves and warns). Keeps the others."""
    keep = events()
    for kw in new:
        ev = {"name": "UIT", "hasG": False, "gD": 0, "gM": 0, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jY": "", "jYNum": 0,
              "isAdarII": False, "afterSunset": False, "type": "other", "icon": "", "warn": 0, "image": ""}
        ev.update(kw)
        keep.append(ev)
    code, b = api_post("/api/events/import", keep)
    return json.loads(b)


def find_event(name):
    return next((e for e in events() if e["name"] == name), None)


def remove_test_events(prefix="UIT"):
    for e in events():
        if e["name"].startswith(prefix):
            http(HOME, "/api/events/delete", form={"id": e["id"]})
    http(HOME, "/api/events/empty-bin", data=b"")


def make_files():
    """Test inputs, made fresh each run: a photo, an animated GIF, a 4 s video, a non-picture."""
    from PIL import Image, ImageDraw
    d = OUT / "files"
    d.mkdir(exist_ok=True)
    im = Image.new("RGB", (400, 300))
    g = ImageDraw.Draw(im)
    for x in range(400):
        g.line([(x, 0), (x, 300)], fill=(x * 255 // 400, 90, 255 - x * 255 // 400))
    g.ellipse([120, 60, 280, 240], fill=(255, 220, 0))
    im.save(d / "photo.png")
    frames = []
    for k in range(6):
        f = Image.new("RGB", (120, 80), (20, 20, 40))
        ImageDraw.Draw(f).rectangle([k * 15, 20, k * 15 + 30, 60], fill=(255, 80, 0))
        frames.append(f)
    frames[0].save(d / "anim.gif", save_all=True, append_images=frames[1:], duration=100, loop=0)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=duration=4:size=320x240:rate=25",
                    "-pix_fmt", "yuv420p", "-c:v", "libx264", str(d / "clip.mp4")], check=True)
    (d / "notapic.txt").write_text("not a picture", encoding="utf-8")
    return d


FILESDIR = [None]


def f(name):
    return str(FILESDIR[0] / name)


def choose_file(pg, button_selector, path):
    with pg.expect_file_chooser() as fc:
        pg.click(button_selector)
    fc.value.set_files(path)
    pg.wait_for_timeout(800)


def press_range(pg, selector, keys):
    """A slider moved by keyboard (real input events, as a person using arrows / Home / End)."""
    pg.focus(selector)
    for k in keys:
        pg.keyboard.press(k)
    pg.wait_for_timeout(100)


G_MAXDAYS = [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
H_MONTHS = ["תשרי", "חשוון", "כסלו", "טבת", "שבט", "אדר", "ניסן", "אייר", "סיון", "תמוז", "אב", "אלול"]
H_MAXDAYS = {"תשרי": 30, "חשוון": 30, "כסלו": 30, "טבת": 29, "שבט": 30, "אדר": 30, "ניסן": 30, "אייר": 29, "סיון": 30, "תמוז": 29, "אב": 30, "אלול": 29}


def heb_num(n):
    if n <= 0:
        return ""
    hundreds = ["", "ק", "ר", "ש", "ת", "תק", "תר", "תש", "תת", "תתק"]
    tens = ["", "י", "כ", "ל", "מ", "נ", "ס", "ע", "פ", "צ"]
    units = ["", "א", "ב", "ג", "ד", "ה", "ו", "ז", "ח", "ט"]
    if n >= 1000:
        rest = n % 1000
        return units[n // 1000] + "׳" + (heb_num(rest) if rest else "")
    res = hundreds[n // 100]
    n %= 100
    if n in (15, 16):
        res += "טו" if n == 15 else "טז"
        n = 0
    res += tens[n // 10]
    res += units[n % 10]
    return res[:-1] + "״" + res[-1] if len(res) > 1 else res + "׳"


def open_event(pg, name, timeout=8):
    """The event dialog of the event called name (the page's list reloaded until it has it)."""
    def idx():
        pg.evaluate("loadEvents()")
        i = pg.evaluate("n => events.findIndex(e => e.name === n)", name)
        return i if i >= 0 else None
    i = wait(idx, timeout, 0.5)
    if i is None:
        raise RuntimeError("event %r not on the page" % name)
    pg.evaluate("i => editEvent(i)", i)
    pg.wait_for_timeout(500)


def modal_open(pg, mid="modal-event"):
    return "open" in (pg.get_attribute("#" + mid, "class") or "")


# ── the sections ──────────────────────────────────────────────────────────────
@section
def calendar_view(ui, clock):
    """Calendar: month arrows and list, Greg/Heb, Current Month, day rows, event chips, search, column split, message box."""
    pg = ui.page
    today = datetime.date.today()
    st = status()
    add_event(name="UIT Chip", hasG=True, gD=20, gM=today.month, gY=1990, type="birthday", icon="🎂")
    add_event(name="UIT HebChip", hasJ=True, jD=10, jMonth=st["todayHMonth"], jYNum=5750, jY="תש״נ", type="other", icon="🗓️")
    ui.goto(HOME + "/")
    idx = lambda: pg.eval_on_selector("#month-select", "s => s.selectedIndex")
    rows = lambda: pg.locator(".dayrow").count()
    check("opens on this month", idx() == today.month - 1, idx())
    pg.locator("button.arrow").nth(1).click()
    check("› = next month", idx() == today.month % 12, idx())
    pg.locator("button.arrow").nth(0).click()
    pg.locator("button.arrow").nth(0).click()
    check("‹ ‹ = month before", idx() == (today.month - 2) % 12, idx())
    bad = []
    for i in range(12):
        pg.select_option("#month-select", str(i))
        if rows() != G_MAXDAYS[i] or idx() != i:
            bad.append((i, rows()))
    check("every Gregorian month from the list, with its days", not bad, bad)
    pg.click("text=Current Month")
    check("Current Month (Gregorian)", idx() == today.month - 1, idx())
    chip = pg.locator(".pill", has_text="UIT Chip")
    check("an event's chip on its day, with the age", chip.count() == 1 and "(%d)" % (today.year - 1990 - (1 if today.day < 20 else 0)) in chip.inner_text(), chip.all_inner_texts())
    chip.click()
    pg.wait_for_timeout(300)
    check("its chip opens it for editing", modal_open(pg) and pg.input_value("#ev-name") == "UIT Chip" and pg.inner_text("#modal-title") == "Edit Event")
    pg.click("#modal-event .close-x")
    check("✕ closes the dialog", not modal_open(pg))
    # Hebrew view
    pg.click("#btn-view-h")
    pg.wait_for_timeout(300)
    check("Heb: days in letters", pg.locator(".daylbl").first.inner_text() == "א׳", pg.locator(".daylbl").first.inner_text())
    bad = []
    for i in range(12):
        pg.select_option("#month-select", str(i))
        if rows() != H_MAXDAYS[H_MONTHS[i]]:
            bad.append((i, rows()))
    check("every Hebrew month from the list, with its days", not bad, bad)
    pg.click("text=Current Month")
    check("Current Month (Hebrew) = the clock's Hebrew month", H_MONTHS[idx()] == st["todayHMonth"], idx())
    check("today's row marked (Hebrew)", pg.locator(".dayrow.today .daylbl").inner_text() == heb_num(st["todayHDay"]))
    hchip = pg.locator(".pill", has_text="UIT HebChip")
    check("a Hebrew-date event on its Hebrew day", hchip.count() == 1)
    pg.locator("button.arrow").nth(1).click()
    check("› in Hebrew view", H_MONTHS[idx()] == H_MONTHS[(H_MONTHS.index(st["todayHMonth"]) + 1) % 12])
    pg.click("#btn-view-g")
    pg.wait_for_timeout(300)
    check("back to Gregorian: days in numbers", pg.locator(".daylbl").first.inner_text() == "1")
    ui.settings()
    check("the Settings tab", pg.is_visible("#view-settings") and not pg.is_visible("#view-calendar"))
    ui.calendar()
    check("the Calendar tab", pg.is_visible("#view-calendar") and not pg.is_visible("#view-settings"))
    pg.click("text=Current Month")
    check("today's row marked (Gregorian)", pg.locator(".dayrow.today .daylbl").inner_text() == str(today.day))
    # A day's row: a new event on that day
    pg.locator(".dayrow").nth(4).click()
    pg.wait_for_timeout(800)
    check("a day's row: new event on that day", modal_open(pg) and pg.input_value("#ev-g-d") == "5" and pg.inner_text("#modal-title") == "New Event")
    pg.locator("#modal-event button", has_text="Cancel").click()
    check("Cancel closes the dialog", not modal_open(pg))
    # Search
    pg.fill("#search-input", "UIT Ch")
    pg.wait_for_timeout(200)
    items = pg.locator("#search-results .search-item")
    check("search finds by name", items.count() == 1 and "UIT Chip" in items.first.inner_text(), items.all_inner_texts())
    items.first.click()
    pg.wait_for_timeout(300)
    check("a result opens that event", modal_open(pg) and pg.input_value("#ev-name") == "UIT Chip" and pg.input_value("#search-input") == "")
    pg.keyboard.press("Escape")
    check("Escape closes the dialog", not modal_open(pg))
    pg.fill("#search-input", "UIT")
    pg.wait_for_timeout(200)
    check("search: several results", pg.locator("#search-results .search-item").count() == 2)
    pg.fill("#search-input", "zzqqxx")
    pg.wait_for_timeout(200)
    check("search: no match says so", pg.inner_text("#search-results") == "No matches")
    pg.locator("#search-results .search-item").click()
    pg.fill("#search-input", "")
    pg.wait_for_timeout(200)
    check("search cleared: list gone", not pg.is_visible("#search-results"))
    # Column split: dragged, remembered
    h = pg.locator(".cal-resizer").bounding_box()
    left0 = pg.locator("[data-cal-left]").bounding_box()["width"]
    pg.mouse.move(h["x"] + h["width"] / 2, h["y"] + 40)
    pg.mouse.down()
    pg.mouse.move(h["x"] + h["width"] / 2 + 150, h["y"] + 40, steps=8)
    pg.mouse.up()
    left1 = pg.locator("[data-cal-left]").bounding_box()["width"]
    check("column split dragged", left1 > left0 + 100, (left0, left1))
    ui.goto(HOME + "/")
    left2 = pg.locator("[data-cal-left]").bounding_box()["width"]
    check("column split remembered after a reload", abs(left2 - left1) < 5, (left1, left2))
    # A message: next to what was clicked, pointing at it; it stays; a click on it, or anywhere else, closes it
    btn = pg.locator("button.rail-item[data-view=calendar]")
    btn.click()
    pg.evaluate("toast('a message')")
    pg.wait_for_timeout(300)
    tb, bb = pg.locator("#toast").bounding_box(), btn.bounding_box()
    near = abs((tb["y"] if tb["y"] > bb["y"] else tb["y"] + tb["height"]) - (bb["y"] + bb["height"] if tb["y"] > bb["y"] else bb["y"])) < 20
    check("a message appears next to what was clicked", near, (tb, bb))
    pg.wait_for_timeout(6000)
    check("... and stays (6 s later still there)", "show" in (pg.get_attribute("#toast", "class") or ""))
    pg.evaluate("el('toast').classList.add('test-hit')")
    pg.click("#toast")
    pg.wait_for_timeout(300)
    check("a click on it closes it", "show" not in (pg.get_attribute("#toast", "class") or ""))
    pg.evaluate("el('toast').classList.remove('test-hit'); toast('another')")
    pg.wait_for_timeout(600)
    pg.mouse.click(640, 12)   # the header bar: nothing there to open
    pg.wait_for_timeout(300)
    check("... so does a click anywhere else", "show" not in (pg.get_attribute("#toast", "class") or ""))
    # A click beside the dialog closes it
    pg.click("button.fab")
    pg.wait_for_timeout(500)
    pg.mouse.click(5, 450)
    pg.wait_for_timeout(300)
    check("a click beside the dialog closes it", not modal_open(pg))
    remove_test_events()


@section
def event_editor(ui, clock):
    """Event dialog: every field, type, icon, both calendars, checks, save, edit, delete + Undo."""
    pg = ui.page
    ui.goto(HOME + "/")
    pg.click("button.fab")
    pg.wait_for_timeout(1000)
    # name required
    pg.fill("#ev-name", "")
    check("no name: refused, field marked", "Enter a name" in ui.click_msg("#modal-event button:has-text('Save')") and "field-missing" in pg.get_attribute("#ev-name", "class"))
    pg.fill("#ev-name", "UIT Full")
    check("typing the name clears the mark", "field-missing" not in pg.get_attribute("#ev-name", "class"))
    # type -> icon
    want = {"birthday": "🎂", "wedding": "💍", "yahrzeit": "🕯️", "other": "🗓️"}
    got = {}
    for t in ["wedding", "yahrzeit", "other", "birthday"]:
        pg.select_option("#ev-type", t)
        got[t] = pg.input_value("#ev-icon")
    check("each type brings its icon", got == want, got)
    # icons
    got = []
    for o in pg.locator("#icon-quick-picker .icon-option").all():
        o.click()
        got.append((o.get_attribute("data-icon"), pg.input_value("#ev-icon"), "sel" in o.get_attribute("class")))
    check("each icon can be chosen", all(a == b and s for a, b, s in got), got)
    pg.fill("#ev-icon-custom", "★")
    check("own icon typed", pg.input_value("#ev-icon") == "★" and pg.locator("#icon-quick-picker .icon-option.sel").count() == 0)
    pg.fill("#ev-warn", "3")
    # Gregorian: every month, then 14 Mar 1990 -> Hebrew follows
    for m in range(13):
        pg.select_option("#ev-g-m", str(m))
    pg.fill("#ev-g-d", "14")
    pg.select_option("#ev-g-m", "3")
    pg.fill("#ev-g-y", "1990")
    pg.wait_for_timeout(1000)
    h = convert(**{"from": "g", "y": 1990, "m": 3, "d": 14})
    check("Gregorian typed: Hebrew filled in", pg.input_value("#ev-j-m") == h["month"] and pg.input_value("#ev-j-d") == heb_num(h["d"])
          and pg.input_value("#ev-j-y") == heb_num(h["y"] - 5000), (pg.input_value("#ev-j-d"), pg.input_value("#ev-j-m"), pg.input_value("#ev-j-y"), h))
    check("typing a date ticks its calendar", pg.is_checked("#ev-has-g"))
    pg.check("#ev-as")
    pg.wait_for_timeout(1000)
    h2 = convert(**{"from": "g", "y": 1990, "m": 3, "d": 14, "afterSunset": True})
    check("After sunset: the next Hebrew day", pg.input_value("#ev-j-d") == heb_num(h2["d"]), (pg.input_value("#ev-j-d"), h2))
    pg.uncheck("#ev-as")
    pg.wait_for_timeout(1000)
    check("After sunset off: back", pg.input_value("#ev-j-d") == heb_num(h["d"]))
    # Hebrew side: every month; Adar shows Adar II
    for m in [""] + H_MONTHS:
        pg.select_option("#ev-j-m", m)
        if m == "אדר":
            check("Adar: the Adar II box appears", pg.is_visible("#row-adar2"))
            pg.check("#ev-is-adar2")
            pg.uncheck("#ev-is-adar2")
    check("other months: no Adar II box", not pg.is_visible("#row-adar2"))
    # Hebrew typed in letters, normalized on leaving the field; Gregorian follows
    pg.uncheck("#ev-has-g")
    pg.fill("#ev-j-d", "כג")
    pg.select_option("#ev-j-m", "ניסן")
    pg.fill("#ev-j-y", "תשנ")
    pg.click("#ev-name")
    pg.wait_for_timeout(1000)
    g = convert(**{"from": "h", "y": 5750, "month": "ניסן", "d": 23})
    check("Hebrew typed in letters, shown as letters", pg.input_value("#ev-j-d") == "כ״ג" and pg.input_value("#ev-j-y") == heb_num(750))
    check("Hebrew typed: Gregorian filled in", (pg.input_value("#ev-g-d"), pg.input_value("#ev-g-m"), pg.input_value("#ev-g-y")) == (str(g["d"]), str(g["m"]), str(g["y"])), g)
    check("Gregorian box stays as set (off)", not pg.is_checked("#ev-has-g") and pg.is_checked("#ev-has-j"))
    pg.check("#ev-has-g")
    pg.click("#modal-event button:has-text('Save')")
    ui.wait_toast()
    e = wait(lambda: find_event("UIT Full"), 5)
    check("saved with every field", e and (e["gD"], e["gM"], e["gY"], e["jD"], e["jMonth"], e["jYNum"], e["type"], e["icon"], e["warn"], e["afterSunset"])
          == (g["d"], g["m"], g["y"], 23, "ניסן", 5750, "birthday", "★", 3, False), e)
    check("dialog closed after saving", not modal_open(pg))
    # The Hebrew day / year: arrows (and the keyboard's), counted as numbers, shown in letters;
    # what isn't a Hebrew day / year is refused, not dropped
    spin = lambda field, d: pg.click(".hspin:has(#%s) button[data-step='%d']" % (field, d))
    pg.click("button.fab")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-name", "UIT Spin")
    pg.fill("#ev-j-d", "טו"); pg.select_option("#ev-j-m", "ניסן"); pg.fill("#ev-j-y", "תשנ"); pg.click("#ev-name")
    pg.wait_for_timeout(600)
    spin("ev-j-d", 1)
    pg.wait_for_timeout(600)
    g16 = convert(**{"from": "h", "y": 5750, "month": "ניסן", "d": 16})
    check("Hebrew day ▴: 15 -> ט״ז, the Gregorian date follows", pg.input_value("#ev-j-d") == "ט״ז" and pg.input_value("#ev-g-d") == str(g16["d"]),
          (pg.input_value("#ev-j-d"), pg.input_value("#ev-g-d"), g16))
    spin("ev-j-d", -1); spin("ev-j-d", -1)
    check("... ▾ twice: י״ד", pg.input_value("#ev-j-d") == "י״ד", pg.input_value("#ev-j-d"))
    spin("ev-j-y", 1)
    check("Hebrew year ▴: תשנ״א", pg.input_value("#ev-j-y") == "תשנ״א", pg.input_value("#ev-j-y"))
    spin("ev-j-y", -1)
    check("... ▾: תש״נ again", pg.input_value("#ev-j-y") == "תש״נ", pg.input_value("#ev-j-y"))
    pg.focus("#ev-j-d"); pg.keyboard.press("ArrowUp")
    check("the keyboard's ↑ in the day box: ט״ו", pg.input_value("#ev-j-d") == "ט״ו", pg.input_value("#ev-j-d"))
    for field, typed, words in [("ev-j-d", "מד", "is not a Hebrew day"), ("ev-j-d", "אט", "is not a Hebrew day")]:
        pg.fill("#" + field, typed)
        ui.clear_toast()
        pg.click("#ev-name")
        said = ui.wait_toast()
        check("%s typed in the %s box: red, says why" % (typed, "day" if field == "ev-j-d" else "year"),
              words in said and "field-missing" in (pg.get_attribute("#" + field, "class") or ""), said)
        said = ui.click_msg("#modal-event button:has-text('Save')")
        check("... and Save refuses it", "Not saved" in said and words in said and modal_open(pg) and not find_event("UIT Spin"), said)
        pg.fill("#" + field, "טו" if field == "ev-j-d" else "תשנ")
        pg.click("#ev-name")
    # A Gregorian year in the Hebrew year box: "Did you mean ...?" (that date in 2027 is in 5787)
    gd, gm = int(pg.input_value("#ev-g-d")), int(pg.input_value("#ev-g-m"))
    want = convert(**{"from": "g", "y": 2027, "m": gm, "d": gd})["y"]
    pg.fill("#ev-j-y", "2027")
    pg.click("#ev-name")
    pg.wait_for_timeout(800)
    check("2027 in the Hebrew year box: asks 'Did you mean %s?'" % heb_num(want - 5000),
          modal_open(pg, "modal-confirm") and ("Did you mean " + heb_num(want - 5000)) in pg.inner_text("#cf-title"), pg.inner_text("#cf-title"))
    ui.ask_answer("Cancel")
    pg.wait_for_timeout(300)
    check("... Cancel: left as typed, red", pg.input_value("#ev-j-y") == "2027" and "field-missing" in (pg.get_attribute("#ev-j-y", "class") or ""))
    pg.click("#modal-event button:has-text('Save')")
    pg.wait_for_timeout(800)
    check("... Save asks again", modal_open(pg, "modal-confirm") and "Did you mean" in pg.inner_text("#cf-title"))
    ui.clear_toast()
    ui.ask_answer("Cancel")
    said = ui.wait_toast()
    check("... Cancel: not saved, says why", "Not saved" in said and "looks like a Gregorian year" in said and not find_event("UIT Spin"), said)
    pg.click("#modal-event button:has-text('Save')")
    pg.wait_for_timeout(800)
    ui.ask_answer("Yes - " + heb_num(want - 5000))
    e = wait(lambda: find_event("UIT Spin"), 8)
    check("... Yes: put in and saved as %d" % want, e and e["jYNum"] == want and not modal_open(pg), e)
    # Dates that don't match. With a year on one side only, the other side is
    # not filled in by itself, so a slip can be typed (the user, 2026-11-07).
    save = "#modal-event button:has-text('Save')"
    def new_event(name):
        pg.click("button.fab")
        pg.wait_for_timeout(1000)
        pg.fill("#ev-name", name)
    # only the Gregorian year: 14 Mar 1990 = 17 Adar; the Hebrew year cleared, day typed 20
    new_event("UIT OnlyG")
    pg.fill("#ev-g-d", "14"); pg.select_option("#ev-g-m", "3"); pg.fill("#ev-g-y", "1990")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-j-y", ""); pg.fill("#ev-j-d", "20"); pg.click("#ev-name")
    pg.wait_for_timeout(500)
    said = ui.click_msg(save)
    check("typed: Gregorian with year, Hebrew day wrong (no Hebrew year): refused", "Date mismatch" in said and "י״ז אדר" in said and modal_open(pg), said)
    pg.fill("#ev-j-d", "17"); pg.click("#ev-name")
    pg.click(save)
    pg.wait_for_timeout(1500)
    e = find_event("UIT OnlyG")
    check("... corrected: saved", e and e["jD"] == 17 and e["jYNum"] == 0 and e["gY"] == 1990, e)
    # only the Hebrew year: 17 Adar 5750 = 14 Mar; the Gregorian year cleared, day typed 20
    new_event("UIT OnlyH")
    pg.fill("#ev-j-d", "17"); pg.select_option("#ev-j-m", "אדר"); pg.fill("#ev-j-y", "5750"); pg.click("#ev-name")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-g-y", ""); pg.fill("#ev-g-d", "20")
    pg.wait_for_timeout(500)
    said = ui.click_msg(save)
    check("typed: Hebrew with year, Gregorian day wrong (no Gregorian year): refused", "Date mismatch" in said and "14/3" in said and modal_open(pg), said)
    # ... the evening before (13 Mar): offers After sunset
    pg.fill("#ev-g-d", "13")
    pg.wait_for_timeout(300)
    ui.dialog_answer[0] = False
    n0 = len(ui.dialogs)
    said = ui.click_msg(save)
    check("... Gregorian = the evening before: offers After sunset (No = not saved)", any("After Sunset" in m for _, m in ui.dialogs[n0:]) and "Date mismatch" in said and modal_open(pg),
          (ui.dialogs[n0:], said))
    ui.dialog_answer[0] = True
    pg.click(save)
    pg.wait_for_timeout(1500)
    e = find_event("UIT OnlyH")
    check("... Yes: saved with After sunset", e and e["afterSunset"] is True and e["gD"] == 13 and e["gY"] == 0, e)
    # no year at all: the clock checks the two can meet in some year (19-year cycle)
    def no_years(name, gd, gm, jd, jm):
        new_event(name)
        pg.fill("#ev-g-y", ""); pg.fill("#ev-j-y", "")
        pg.fill("#ev-g-d", str(gd)); pg.select_option("#ev-g-m", str(gm))
        pg.fill("#ev-j-d", str(jd)); pg.select_option("#ev-j-m", jm); pg.click("#ev-name")
        pg.check("#ev-has-g"); pg.check("#ev-has-j")
        pg.wait_for_timeout(300)
    no_years("UIT NoYearsBad", 5, 1, 5, "ניסן")
    said = ui.click_msg(save)
    check("typed: no years, January + Nisan (never the same day): refused, dialog open",
          "never fall on the same day" in said and "ה׳ ניסן" in said and modal_open(pg) and not find_event("UIT NoYearsBad"), said)
    pg.locator("#modal-event button", has_text="Cancel").click()
    no_years("UIT NoYears", 14, 3, 20, "אדר")
    pg.click(save)
    pg.wait_for_timeout(1500)
    e = find_event("UIT NoYears")
    check("typed: no years, 14 March + 20 Adar (meet in some years): saved", e and (e["gD"], e["gM"], e["jD"], e["jMonth"]) == (14, 3, 20, "אדר"), e)
    # Stored that way (an Excel import saves and warns): a wrong day, a wrong year
    r = store_events(dict(name="UIT Mismatch", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h["d"] + 3, jMonth=h["month"], jYNum=5750, jY="תש״נ"),
                     dict(name="UIT WrongYear", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h["d"], jMonth=h["month"], jYNum=5751, jY="תשנ״א"))
    check("an import stores mismatched dates, with a warning each", r.get("saved") and len(r.get("warnings", [])) == 2, r)
    add_event(name="UIT Sunset", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h2["d"], jMonth=h2["month"], jYNum=5750, jY="תש״נ")
    for name, what in [("UIT Mismatch", "a wrong day"), ("UIT WrongYear", "a wrong year")]:
        open_event(pg, name)
        said = ui.click_msg(save)
        check("stored with %s: Save refused" % what, "Date mismatch" in said and modal_open(pg), said)
        pg.locator("#modal-event button", has_text="Cancel").click()
    e = find_event("UIT Sunset")
    check("the clock ticks After sunset itself for an evening Hebrew date", e and e["afterSunset"] is True, e)
    pg.click("button.fab")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-name", "UIT NoCal")
    pg.uncheck("#ev-has-g")
    pg.uncheck("#ev-has-j")
    pg.check("#ev-has-j")
    pg.uncheck("#ev-has-j")
    check("nothing ticked: says to tick a calendar", "Tick Gregorian, Hebrew or both" in ui.click_msg("#modal-event button:has-text('Save')")
          and find_event("UIT NoCal") is None)
    pg.locator("#modal-event button", has_text="Cancel").click()
    # Incomplete: a calendar ticked with no month
    pg.click("button.fab")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-name", "UIT Half")
    pg.fill("#ev-j-d", "5")
    pg.select_option("#ev-j-m", "ניסן")
    pg.wait_for_timeout(800)
    pg.select_option("#ev-g-m", "0")
    pg.check("#ev-has-g")
    pg.wait_for_timeout(300)
    check("a ticked calendar without a month: refused", "incomplete" in ui.click_msg("#modal-event button:has-text('Save')") and find_event("UIT Half") is None)
    pg.locator("#modal-event button", has_text="Cancel").click()
    # Edit: rename, save
    open_event(pg, "UIT Full")
    pg.fill("#ev-name", "UIT Full Renamed")
    pg.fill("#ev-warn", "0")
    pg.click("#modal-event button:has-text('Save')")
    ui.wait_toast()
    e = wait(lambda: find_event("UIT Full Renamed"), 5)
    check("edit: renamed, same event", e and find_event("UIT Full") is None and e["warn"] == 0, e)
    # Delete: askBox Cancel, then Delete, then Undo
    open_event(pg, "UIT Full Renamed")
    pg.click("#btn-del")
    pg.wait_for_timeout(300)
    check("Delete asks first", modal_open(pg, "modal-confirm") and "UIT Full Renamed" in pg.inner_text("#cf-title"), pg.inner_text("#cf-title"))
    ui.ask_answer("Cancel")
    check("... Cancel: kept", find_event("UIT Full Renamed") is not None and modal_open(pg))
    pg.click("#btn-del")
    pg.wait_for_timeout(300)
    ui.ask_answer("Delete")
    pg.wait_for_timeout(800)
    check("... Delete: gone, in the recycle bin", find_event("UIT Full Renamed") is None and any(x["name"] == "UIT Full Renamed" for x in jget("/api/events/deleted")))
    pg.locator("#toast button", has_text="Undo").click()
    pg.wait_for_timeout(1000)
    check("Undo brings it back", find_event("UIT Full Renamed") is not None and not any(x["name"] == "UIT Full Renamed" for x in jget("/api/events/deleted")))
    remove_test_events()

@section
def event_pictures(ui, clock):
    """An event's picture: add, zoom, move, replace, remove, a video's moment, the library, a bad file."""
    pg = ui.page
    ui.goto(HOME + "/")
    lib0 = {x["id"] for x in jget("/api/images/list")}
    pg.click("button.fab")
    pg.wait_for_timeout(800)
    pg.fill("#ev-name", "UIT Pic")
    pg.check("#ev-has-g")
    choose_file(pg, "#ev-photo-empty", f("photo.png"))
    check("a photo: shown, with zoom / replace / remove", pg.is_visible("#ev-photo-canvas") and pg.is_visible("#ev-photo-zoom")
          and pg.is_visible("#ev-photo-change") and pg.is_visible("#ev-photo-remove") and not pg.is_visible("#ev-photo-empty"))
    px = lambda: pg.evaluate("Array.from(el('ev-photo-canvas').getContext('2d').getImageData(0,0,PHOTO_W,PHOTO_H).data.slice(0, 4000)).join(',')")
    a = px()
    press_range(pg, "#ev-photo-zoom", ["ArrowRight"] * 30)
    b = px()
    check("zoom changes the picture", a != b and pg.evaluate("photo.zoom") > 1.2, pg.evaluate("photo.zoom"))
    box = pg.locator("#ev-photo-canvas").bounding_box()
    pg.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
    pg.mouse.down()
    pg.mouse.move(box["x"] + box["width"] / 2 + 25, box["y"] + box["height"] / 2 + 15, steps=5)
    pg.mouse.up()
    check("dragging moves the picture in the frame", px() != b and (pg.evaluate("photo.ox") != 0 or pg.evaluate("photo.oy") != 0))
    choose_file(pg, "#ev-photo-change", f("anim.gif"))
    check("✎ replaces it (a GIF: its first picture)", pg.evaluate("!!photo.img && photo.img.naturalWidth === 120"))
    pg.click("#ev-photo-remove")
    check("✕ removes it", pg.is_visible("#ev-photo-empty") and not pg.is_visible("#ev-photo-canvas"))
    choose_file(pg, "#ev-photo-empty", f("notapic.txt"))
    check("not a picture: says so in the dialog", "not a picture or video" in pg.inner_text("#ev-photo-msg"), pg.inner_text("#ev-photo-msg"))
    # A video: the picture is the moment shown
    choose_file(pg, "#ev-photo-empty", f("clip.mp4"))
    pg.wait_for_timeout(1500)
    trim = "#ev-photo-trim"
    check("a video: its player shows, the message is gone", pg.is_visible(trim + " video") and not pg.is_visible("#ev-photo-msg"))
    t = lambda: pg.evaluate("document.querySelector('#ev-photo-trim video').currentTime")
    for a_, want in [("+1", 1.0), ("+0.1", 1.1), ("-0.1", 1.0), ("-1", 0.0)]:
        pg.click(trim + ' button[data-a="%s"]' % a_)
        pg.wait_for_timeout(400)
        check("video %s s" % a_, abs(t() - want) < 0.05, t())
    pg.fill(trim + " .trim-time", "0:02.5")
    pg.press(trim + " .trim-time", "Enter")
    pg.wait_for_timeout(500)
    check("a typed moment (0:02.5 + Enter)", abs(t() - 2.5) < 0.05, t())
    press_range(pg, trim + " .trim-bar input", ["ArrowLeft"] * 50)
    pg.wait_for_timeout(500)
    check("the moment slider", t() < 2.4, t())
    pg.click(trim + ' button[data-a="play"]')
    pg.wait_for_timeout(700)
    playing = not pg.evaluate("document.querySelector('#ev-photo-trim video').paused")
    pg.click(trim + ' button[data-a="play"]')
    check("▶ plays, ❚❚ stops", playing and pg.evaluate("document.querySelector('#ev-photo-trim video').paused"))
    pg.wait_for_timeout(500)
    check("the picture is the moment shown", pg.evaluate("!!photo.img && photo.changed"))
    pg.click("#modal-event button:has-text('Save')")
    pg.wait_for_timeout(2000)
    e = find_event("UIT Pic")
    lib1 = {x["id"] for x in jget("/api/images/list")}
    check("saved: the picture went to the clock's library", e and e["image"] and e["image"] in lib1 and e["image"] not in lib0, (e, lib1 - lib0))
    code, bmp = http(HOME, "/api/images/get?id=" + (e or {}).get("image", "x"), raw=True)
    ph = jget("/api/limits")["pictureMaxH"]; pw = round(ph * 3 / 4)
    check("... as tall as the clock's window shows a picture (%dx%d)" % (pw, ph), code == 200 and bmp[:2] == b"BM"
          and int.from_bytes(bmp[18:22], "little") == pw and abs(int.from_bytes(bmp[22:26], "little", signed=True)) == ph,
          (int.from_bytes(bmp[18:22], "little"), int.from_bytes(bmp[22:26], "little", signed=True)))
    # The library: another event uses the same picture
    pg.click("button.fab")
    pg.wait_for_timeout(800)
    pg.fill("#ev-name", "UIT Pic2")
    pg.check("#ev-has-g")
    pg.click("text=Library")
    pg.wait_for_timeout(1500)
    items = pg.locator("#ev-gallery .gal-item")
    check("Library lists the clock's pictures", items.count() == len(lib1), (items.count(), len(lib1)))
    pg.click("text=Library")
    check("Library again closes it", not pg.is_visible("#ev-gallery"))
    pg.click("text=Library")
    pg.wait_for_timeout(1000)
    k = [i for i, x in enumerate(jget("/api/images/list")) if x["id"] == e["image"]][0]
    items.nth(k).click()
    pg.wait_for_timeout(800)
    check("a library picture chosen", pg.evaluate("photo.imageId") == e["image"] and not pg.is_visible("#ev-gallery"))
    pg.click("#modal-event button:has-text('Save')")
    pg.wait_for_timeout(1500)
    e2 = find_event("UIT Pic2")
    check("... saved: the same picture, not a copy", e2 and e2["image"] == e["image"] and {x["id"] for x in jget("/api/images/list")} == lib1)
    # Settings > Pictures: every event picture, with the events that use it
    def pic_tile():
        ui.settings()
        return pic_tile_here()
    def pic_tile_here():
        lib = jget("/api/images/list")
        tiles = pg.locator("#evpic-grid .builtin-item")
        wait(lambda: tiles.count() == len(lib), 10, 0.3)
        k = [i for i, x in enumerate(lib) if x["id"] == e["image"]][0]
        pg.wait_for_timeout(800)   # the list is redrawn after a change
        return lib, tiles, (tiles.nth(k).inner_text().splitlines() if tiles.count() > k else []), k
    lib, tiles, lines, k = pic_tile()
    left = jget("/api/images/room")["leftBytes"]
    room = pg.inner_text("#evpic-room")
    names = [l.split(" ", 1)[1] for l in lines if " " in l]   # "<type icon> <name>"
    check("Settings > Pictures lists every event picture", tiles.count() == len(lib), (tiles.count(), len(lib)))
    check("... ours with both its events, each with its type's icon", "UIT Pic" in names and "UIT Pic2" in names
          and any(l.endswith(" UIT Pic") and not l[0].isalnum() for l in lines), lines)
    per = sum(x["bytes"] for x in lib) / len(lib)
    check("... how many pictures, next to the header", room == "%d picture%s" % (len(lib), "" if len(lib) == 1 else "s"), room)
    bl = jget("/api/builtin/list")
    ev_b, ic_b = sum(x["bytes"] for x in lib), bl["usedBytes"]
    total = ev_b + ic_b + left
    bar_ev = pg.evaluate("parseFloat(el('st-bar-ev').style.width)")
    check("the space bar: the event pictures' part as the clock says", abs(bar_ev - 100 * ev_b / total) < 0.05, (bar_ev, ev_b, total))
    check("... free, and about how many more pictures", ("about %d more pictures" % int(left // per)) in pg.inner_text("#st-free"), pg.inner_text("#st-free"))
    drawn = wait(lambda: pg.evaluate("i => { const c = document.querySelectorAll('#evpic-grid canvas')[i]; return Array.from(c.getContext('2d').getImageData(0,0,PHOTO_W,PHOTO_H).data).some((v, j) => j % 4 != 3 && v > 0); }", k), 10, 0.3)
    check("... the picture itself drawn", drawn)
    # A name opens its event over Settings; Cancel / Save / Delete leave you on Settings
    link = lambda n: pg.locator("#evpic-grid a[data-open-event]", has_text=n).first
    on_settings = lambda: pg.is_visible("#view-settings") and not pg.is_visible("#view-calendar")
    link("UIT Pic2").click()
    pg.wait_for_timeout(800)
    check("a name opens that event", modal_open(pg) and pg.input_value("#ev-name") == "UIT Pic2", pg.input_value("#ev-name"))
    pg.locator("#modal-event button", has_text="Cancel").click()
    pg.wait_for_timeout(500)
    check("... Cancel: back on Settings", not modal_open(pg) and on_settings())
    link("UIT Pic2").click()
    pg.wait_for_timeout(800)
    pg.fill("#ev-name", "UIT Pic2 Renamed")
    pg.click("#modal-event button:has-text('Save')")
    check("... Save: back on Settings, the new name listed", wait(lambda: not modal_open(pg) and on_settings() and link("UIT Pic2 Renamed").count() == 1, 10, 0.3)
          and find_event("UIT Pic2 Renamed"))
    link("UIT Pic2 Renamed").click()
    pg.wait_for_timeout(800)
    pg.click("#btn-del")
    ui.ask_answer("Delete")
    lib, tiles, lines, k = pic_tile_here()
    check("... Delete: back on Settings, the event listed as (deleted)", on_settings() and not modal_open(pg)
          and any(l.endswith("UIT Pic2 Renamed (deleted)") for l in lines), lines)
    http(HOME, "/api/events/restore", form={"id": e2["id"]})
    ui.calendar()
    # Opening an event with a picture shows it
    open_event(pg, "UIT Pic")
    check("an event's picture shows when it's opened", pg.is_visible("#ev-photo-canvas") and pg.evaluate("photo.imageId") == e["image"])
    pg.click("#ev-photo-remove")
    pg.click("#modal-event button:has-text('Save')")
    pg.wait_for_timeout(1500)
    check("picture removed and saved: the event has none", find_event("UIT Pic")["image"] == "")
    remove_test_events()
    pg.wait_for_timeout(500)
    check("pictures no event uses leave the library", e["image"] not in {x["id"] for x in jget("/api/images/list")})


@section
def recycle_bin(ui, clock):
    """Deleted events: listed, Restore one, tick + Restore selected, Restore all, Empty recycle bin."""
    pg = ui.page
    remove_test_events()
    ids = [add_event(name="UIT Bin%d" % i, hasG=True, gD=i + 1, gM=3) for i in range(4)]
    for i in ids:
        http(HOME, "/api/events/delete", form={"id": i})
    ui.goto(HOME + "/")
    ui.settings()
    pg.wait_for_timeout(1000)
    rows = pg.locator("#bin-list .frow")
    check("Deleted events lists them, newest first", rows.count() == 4 and "UIT Bin3" in rows.first.inner_text(), rows.all_inner_texts())
    check("... with their dates", "4.3" in rows.first.inner_text())
    check("Restore selected: off until something is ticked", pg.is_disabled("#bin-restore-sel"))
    pg.locator('#bin-list [data-restore="%s"]' % ids[0]).click()
    pg.wait_for_timeout(1000)
    check("Restore (one)", find_event("UIT Bin0") is not None and rows.count() == 3)
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[1])
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.uncheck('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.click("#bin-restore-sel")
    pg.wait_for_timeout(1000)
    check("Restore selected (two)", wait(lambda: find_event("UIT Bin1") and find_event("UIT Bin2") and rows.count() == 1, 8), (rows.count(), [e["name"] for e in events() if "Bin" in e["name"]]))
    pg.click("#bin-restore-all")
    pg.wait_for_timeout(1000)
    check("Restore all", find_event("UIT Bin3") and jget("/api/events/deleted") == [] and "Empty" in pg.inner_text("#bin-list"))
    check("an empty bin: its buttons are off", pg.is_disabled("#bin-restore-all") and pg.is_disabled("#bin-empty"))
    for i in ids[:2]:
        http(HOME, "/api/events/delete", form={"id": i})
    ui.settings()
    pg.wait_for_timeout(800)
    pg.click("#bin-empty")
    check("Empty recycle bin asks first", modal_open(pg, "modal-confirm"))
    ui.ask_answer("Cancel")
    check("... Cancel: still there", len(jget("/api/events/deleted")) == 2)
    pg.click("#bin-empty")
    ui.ask_answer("Empty recycle bin")
    pg.wait_for_timeout(1000)
    check("... emptied", jget("/api/events/deleted") == [] and "Empty" in pg.inner_text("#bin-list"))
    remove_test_events()


@section
def display_settings(ui, clock):
    """Display: room light on/off, both double sliders, learning, fixed brightness (slider + number), message mode, dwell, speed."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    c = cfg()
    if not c["useLightSensor"]:
        pg.click("#cfg-use-sensor + .sl")
    check("Adjust to room light on: its sliders show", wait_cfg("useLightSensor", True) and pg.is_visible("#dr-bright") and not pg.is_visible("#cfg-bright-s"))
    lo, hi = "#dr-bright input >> nth=0", "#dr-bright input >> nth=1"
    press_range(pg, lo, ["Home"] + ["ArrowRight"] * 7)
    check("faintest brightness", wait_cfg("nightPct", 7.0), cfg()["nightPct"])
    check("... its label", pg.inner_text("#dr-bright-lbl").startswith("7%"), pg.inner_text("#dr-bright-lbl"))
    press_range(pg, hi, ["End"] + ["ArrowLeft"] * 10)
    check("brightest", wait_cfg("dayPct", 90.0), cfg()["dayPct"])
    press_range(pg, lo, ["End"])
    pg.wait_for_timeout(500)
    check("the handles can't cross", float(pg.eval_on_selector(lo, "e => e.value")) <= float(pg.eval_on_selector(hi, "e => e.value")))
    press_range(pg, lo, ["Home"] + ["ArrowRight"] * 5)
    rlo, rhi = "#dr-room input >> nth=0", "#dr-room input >> nth=1"
    press_range(pg, rlo, ["Home"] + ["ArrowRight"] * 4)
    press_range(pg, rhi, ["End"] + ["ArrowLeft"] * 20)
    check("room light: dark and light ends", wait_cfg("roomDarkPct", 4.0) and wait_cfg("roomLightPct", 80.0), (cfg()["roomDarkPct"], cfg()["roomLightPct"]))
    was = cfg()["learnRoomLight"]
    pg.click("#cfg-learn-room + .sl")
    check("Learn room light switched", wait_cfg("learnRoomLight", not was))
    pg.click("#cfg-learn-room + .sl")
    check("... and back", wait_cfg("learnRoomLight", was))
    pg.click("#cfg-use-sensor + .sl")
    check("Adjust to room light off: one fixed brightness", wait_cfg("useLightSensor", False) and pg.is_visible("#cfg-bright-s") and not pg.is_visible("#dr-bright"))
    press_range(pg, "#cfg-bright-s", ["Home"] + ["ArrowRight"] * 42)
    check("fixed brightness slider", wait_cfg("brightPct", 42) and pg.input_value("#cfg-bright-n") == "42")
    pg.fill("#cfg-bright-n", "65")
    check("fixed brightness typed", wait_cfg("brightPct", 65) and pg.input_value("#cfg-bright-s") == "65")
    check("... the clock uses it", wait(lambda: abs(status()["brightnessPct"] - 65) < 1, 10), status()["brightnessPct"])
    pg.click("#cfg-use-sensor + .sl")
    wait_cfg("useLightSensor", True)
    pg.click("#rot-scroll")
    check("Single Scroll", wait_cfg("rotationMode", 1) and pg.is_disabled("#cfg-dwell-s"))
    pg.click("#rot-alt")
    check("Alternating (dwell time usable again)", wait_cfg("rotationMode", 0) and not pg.is_disabled("#cfg-dwell-s"))
    press_range(pg, "#cfg-dwell-s", ["Home"] + ["ArrowRight"] * 3)
    check("dwell time", wait_cfg("messageDwellSec", 5) and pg.inner_text("#cfg-dwell-lbl") == "5 s", pg.inner_text("#cfg-dwell-lbl"))
    press_range(pg, "#cfg-scroll-s", ["Home"] + ["ArrowRight"])
    check("scroll speed", wait_cfg("scrollSpeed", 2) and pg.inner_text("#cfg-scroll-lbl") == "2/10")
    pg.wait_for_timeout(2500)
    ui.goto(HOME + "/")
    ui.settings()
    check("after a reload the page shows what the clock has", pg.inner_text("#cfg-scroll-lbl") == "2/10" and pg.inner_text("#cfg-dwell-lbl") == "5 s"
          and pg.is_checked("#cfg-use-sensor") and pg.inner_text("#dr-bright-lbl").startswith("5%"), pg.inner_text("#dr-bright-lbl"))


@section
def calendar_content(ui, clock):
    """Calendar Content: Show year, every row's frequency, its own choices (Omer wording, nusach, days, event day), drag to reorder."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    was = cfg()["showHebrewYear"]
    pg.click("#t-heb-year + .sl")
    check("Show year switched", wait_cfg("showHebrewYear", not was))
    pg.click("#t-heb-year + .sl")
    check("... and back", wait_cfg("showHebrewYear", was))
    keys = pg.evaluate("currentContentOrder()")
    bad = []
    for k in keys:
        sel = '#content-order-list .content-row[data-type="%s"] .content-freq' % k
        for v in ["0", "2", "3", "4", "1"]:
            pg.select_option(sel, v)
            if not wait(lambda: cfg()["contentFrequency"].get(k) == int(v), 6):
                bad.append((k, v, cfg()["contentFrequency"].get(k)))
    check("every row: every frequency (Off .. Every 4th) saved", not bad, bad)
    bad = []
    for cfgkey, values in [("omerMode", [4, 3, 2, 1]), ("nusach", [1, 0]), ("prayerSwitchDays", [0, 2, 1]), ("eventDayChange", [1, 0])]:
        for v in values:
            pg.select_option('#content-order-list select[data-cfg="%s"]' % cfgkey, str(v))
            if not wait_cfg(cfgkey, v, 6):
                bad.append((cfgkey, v, cfg().get(cfgkey)))
    check("every row's own choices saved", not bad, bad)
    handle = lambda k: pg.locator('#content-order-list .content-row[data-type="%s"] .drag-handle' % k)
    first, third = keys[0], keys[2]
    a, b = handle(first).bounding_box(), handle(third).bounding_box()
    pg.mouse.move(a["x"] + a["width"] / 2, a["y"] + a["height"] / 2)
    pg.mouse.down()
    pg.mouse.move(b["x"] + b["width"] / 2, b["y"] + b["height"] / 2 + 6, steps=15)
    pg.mouse.up()
    pg.wait_for_timeout(1500)
    new = pg.evaluate("currentContentOrder()")
    check("dragged to another place", new != keys and new.index(first) > 0, new[:4])
    check("... saved in that order", wait(lambda: cfg()["contentOrder"] == new, 6), cfg()["contentOrder"][:4])
    # back
    a, b = handle(first).bounding_box(), handle(new[0]).bounding_box()
    pg.mouse.move(a["x"] + a["width"] / 2, a["y"] + a["height"] / 2)
    pg.mouse.down()
    pg.mouse.move(b["x"] + b["width"] / 2, b["y"] + 2, steps=15)
    pg.mouse.up()
    pg.wait_for_timeout(1500)
    check("dragged back", wait(lambda: cfg()["contentOrder"] == keys, 6), cfg()["contentOrder"][:4])


@section
def show_windows(ui, clock):
    """Calendar Content: when candles, Havdalah, fast, parasha and Mevarchim show (2026-11-08) -
    every choice saved on the clock, choices that don't apply hidden."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    sel = lambda c: '#content-order-list [data-cfg="%s"]' % c
    shown = lambda selector: pg.locator(selector).is_visible()
    defaults = {k: cfg()[k] for k in ("candleFrom", "candleUntil", "candleTomorrow", "havdalahFrom", "havdalahUntil",
                                      "havdalahTomorrow", "fastFrom", "fastUntil", "parashaFrom", "parashaUntil",
                                      "mevarchimFrom", "mevarchimUntil")}
    check("as it always was until now: candles/Havdalah/fast midnight to midnight, parasha all week, Mevarchim like it",
          defaults == {"candleFrom": 0, "candleUntil": 1, "candleTomorrow": True, "havdalahFrom": 0, "havdalahUntil": 1,
                       "havdalahTomorrow": True, "fastFrom": 0, "fastUntil": 1, "parashaFrom": 2, "parashaUntil": 0,
                       "mevarchimFrom": 3, "mevarchimUntil": 0}, defaults)
    check("'Tomorrow' hidden while 'From midnight'", not shown(sel("candleTomorrow")) and not shown(sel("havdalahTomorrow")))
    check("parasha / Mevarchim 'Until' hidden while 'All week' / 'Like Parasha'",
          not shown(sel("parashaUntil")) and not shown(sel("mevarchimUntil")))

    def choose(key, values):
        bad = []
        for v in values:
            pg.select_option(sel(key), str(v))
            if not wait_cfg(key, v, 6):
                bad.append((v, cfg().get(key)))
        return bad

    bad = []
    for k in ("candle", "havdalah"):
        bad += choose(k + "From", [1])
        if not shown(sel(k + "Tomorrow")):
            bad.append((k, "Tomorrow not shown with 'From evening before'"))
        for want in (False, True):
            pg.click(sel(k + "Tomorrow"))
            if not wait_cfg(k + "Tomorrow", want, 6):
                bad.append((k + "Tomorrow", want, cfg().get(k + "Tomorrow")))
        bad += choose(k + "Until", [0, 1])
        bad += choose(k + "From", [0])
        if shown(sel(k + "Tomorrow")):
            bad.append((k, "Tomorrow still shown with 'From midnight'"))
    bad += choose("fastFrom", [1, 0]) + choose("fastUntil", [0, 1])
    check("candles, Havdalah, fast: every From / Until saved, 'Tomorrow' both ways, shown only with 'evening before'", not bad, bad)

    bad = choose("parashaFrom", [1])
    if not shown(sel("parashaUntil")):
        bad.append("parasha Until not shown")
    bad += choose("parashaUntil", [1, 0]) + choose("parashaFrom", [0, 2])
    if shown(sel("parashaUntil")):
        bad.append("parasha Until shown with 'All week'")
    bad += choose("mevarchimFrom", [2])
    if shown(sel("mevarchimUntil")):
        bad.append("Mevarchim Until shown with 'All week'")
    bad += choose("mevarchimFrom", [1])
    if not shown(sel("mevarchimUntil")):
        bad.append("Mevarchim Until not shown")
    bad += choose("mevarchimUntil", [1, 0]) + choose("mevarchimFrom", [0, 3])
    check("parasha, Mevarchim: every choice saved, 'Until' only when it applies", not bad, bad)

    code, _ = api_post("/api/config", {"candleFrom": 7, "parashaFrom": 3, "fastUntil": 5})
    c = cfg()
    check("the clock refuses a choice a slide doesn't offer (default instead)",
          (c["candleFrom"], c["parashaFrom"], c["fastUntil"]) == (0, 2, 1), (code, c["candleFrom"], c["parashaFrom"], c["fastUntil"]))

    # What the screen shows with each choice, at every kind of day: sim/test_show_windows.py
    # (simulated time - seconds, not the minutes of real time this clock would need).
    api_post("/api/config", defaults)
    check("back to how it was", all(cfg()[k] == v for k, v in defaults.items()))


def builtin(name):
    return next(p for p in jget("/api/builtin/list")["pictures"] if p["name"] == name)


def builtin_item(pg, label):
    return pg.locator("#builtin-grid .builtin-item", has=pg.locator(".bi-name", has_text=label))


@section
def clock_pictures(ui, clock):
    """Settings > Pictures and icons, the icons: Replace with a photo / GIF / part of a video (zoom, width, shape, move, every video button), Original, Cancel, ✕."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    pg.wait_for_timeout(3000)
    check("Pictures: the clock's own, each with Replace", pg.locator("#builtin-grid .builtin-item").count() == len(jget("/api/builtin/list")["pictures"]))
    bl = jget("/api/builtin/list")
    check("Icons: how many replaced; the bar shows their space, of the most they may use",
          pg.inner_text("#bi-room") == "%d of %d replaced" % (sum(1 for p in bl["pictures"] if p.get("custom")), len(bl["pictures"]))
          and ("up to %d KB" % (bl["maxBytes"] // 1024)) in pg.inner_text("#st-ic"), (pg.inner_text("#bi-room"), pg.inner_text("#st-ic")))
    item = builtin_item(pg, "Candle lighting")
    # Cancel / ✕ leave it as it was
    item.locator("button", has_text="Replace").click()
    pg.wait_for_timeout(300)
    check("Replace opens the editor", modal_open(pg, "modal-builtin") and "Candle lighting" in pg.inner_text("#bi-title"))
    check("... Save is off until a picture is chosen", pg.is_disabled("#bi-save"))
    pg.locator("#modal-builtin button", has_text="Cancel").click()
    check("Cancel closes it", not modal_open(pg, "modal-builtin"))
    item.locator("button", has_text="Replace").click()
    pg.wait_for_timeout(300)
    pg.click("#modal-builtin .close-x")
    check("✕ closes it", not modal_open(pg, "modal-builtin") and not builtin("Candles").get("custom"))
    # A photo
    item.locator("button", has_text="Replace").click()
    choose_file(pg, "#modal-builtin button:has-text('Picture or video')", f("notapic.txt"))
    check("not a picture: says so", "not a picture or video" in pg.inner_text("#bi-msg"))
    choose_file(pg, "#modal-builtin button:has-text('Picture or video')", f("photo.png"))
    check("a photo: zoom, the frame's edges and the shape buttons appear", pg.is_visible("#bi-zoom") and pg.is_visible("#bi-shape-row")
          and pg.locator("#modal-builtin .bi-handle:visible").count() == 2 and not pg.is_disabled("#bi-save"))
    check("as tall as the clock's window shows a picture", pg.evaluate("bi.h") == jget("/api/limits")["pictureMaxH"], pg.evaluate("bi.h"))
    w0 = pg.evaluate("bi.w")
    check("its width starts as the photo's shape (4:3 at that height)", abs(w0 - round(pg.evaluate("bi.h") * 4 / 3)) <= 1, w0)
    zoom_at = pg.locator("#bi-zoom").bounding_box()
    scale0 = pg.evaluate("biScale(biSource(0))")
    for side, sign in (("1", -1), ("-1", 1)):   # right edge dragged left, left edge dragged right: both narrower
        hb = pg.locator("#modal-builtin .bi-handle[data-side='%s']" % side).bounding_box()
        x, y = hb["x"] + hb["width"] / 2, hb["y"] + hb["height"] / 2
        before = pg.evaluate("bi.w")
        pg.mouse.move(x, y); pg.mouse.down()
        widths = []
        for i in range(1, 6):
            pg.mouse.move(x + sign * i * 5, y); pg.wait_for_timeout(40); widths.append(pg.evaluate("bi.w"))
        pg.mouse.up()
        check("dragging the frame's %s edge inwards: narrower, step by step" % ("right" if side == "1" else "left"),
              all(a > b for a, b in zip([before] + widths, widths)) and str(widths[-1]) in pg.inner_text("#bi-width-lbl"), (before, widths))
    check("... the controls stay where they are", pg.locator("#bi-zoom").bounding_box() == zoom_at)
    check("... and the picture keeps its size (cropping doesn't zoom)", pg.evaluate("biScale(biSource(0))") == scale0, (scale0, pg.evaluate("biScale(biSource(0))")))
    pg.click("#bi-shape-orig")
    check("Original icon's shape: as wide as the icon it replaces", pg.evaluate("bi.w") == pg.evaluate("bi.origW"), pg.evaluate("[bi.w, bi.origW]"))
    pg.click("#bi-shape-pic")
    check("Picture's own shape: back to the photo's", pg.evaluate("bi.w") == w0)
    press_range(pg, "#bi-zoom", ["ArrowRight"] * 20)
    check("Zoom slider", pg.evaluate("bi.zoom") > 1.3)
    box = pg.locator("#bi-canvas").bounding_box()
    pg.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
    pg.mouse.down()
    pg.mouse.move(box["x"] + box["width"] / 2 + 30, box["y"] + box["height"] / 2, steps=5)
    pg.mouse.up()
    check("dragging moves it in the frame", pg.evaluate("bi.ox") != 0)
    pg.click("#bi-save")
    pg.wait_for_timeout(1500)
    p = builtin("Candles")
    check("saved: the clock has it, a still", p.get("custom") and p.get("customFrames") == 1 and not modal_open(pg, "modal-builtin"), p)
    code, bmp = http(HOME, "/api/builtin/get?name=Candles&which=custom", raw=True)
    check("... the file is the picture", code == 200 and bmp[:2] == b"BM" and int.from_bytes(bmp[18:22], "little") == w0)
    pg.wait_for_timeout(1500)
    item = builtin_item(pg, "Candle lighting")
    check("the grid says yours, with Original", "yours" in item.inner_text() and item.locator("button", has_text="Original").count() == 1)
    # Original: asks, Cancel keeps it, Restore puts the clock's own back
    item.locator("button", has_text="Original").click()
    pg.wait_for_timeout(300)
    check("Original asks first, showing the picture", modal_open(pg, "modal-confirm") and pg.locator("#cf-body canvas").count() == 1)
    ui.ask_answer("Cancel")
    check("... Cancel: still replaced", builtin("Candles").get("custom"))
    item.locator("button", has_text="Original").click()
    ui.ask_answer("Restore")
    pg.wait_for_timeout(1500)
    check("... Restore: the clock's own again", not builtin("Candles").get("custom"))
    # An animated GIF
    builtin_item(pg, "Havdalah").locator("button", has_text="Replace").click()
    choose_file(pg, "#modal-builtin button:has-text('Picture or video')", f("anim.gif"))
    pg.wait_for_timeout(1000)
    check("a GIF: an animation", (pg.evaluate("bi.frames ? bi.frames.length : 1") or 1) > 1 and " s · " in pg.inner_text("#bi-size"), pg.inner_text("#bi-size"))
    pg.click("#bi-save")
    pg.wait_for_timeout(1500)
    p = builtin("Havdala")
    check("... saved as an animation", p.get("custom") and p.get("customFrames", 1) > 1, p)
    # A part of a video: every button of the player
    builtin_item(pg, "Parasha").locator("button", has_text="Replace").click()
    choose_file(pg, "#modal-builtin button:has-text('Picture or video')", f("clip.mp4"))
    pg.wait_for_timeout(3000)
    trim = "#bi-trim"
    vt = lambda: pg.evaluate("document.querySelector('#bi-trim video').currentTime")
    part = lambda: pg.inner_text(trim + " .trim-len")
    check("a video: the player, a part chosen", pg.is_visible(trim + " video") and "s chosen" in part(), part())
    pg.click(trim + ' button[data-a="+1"]')
    pg.click(trim + ' button[data-a="+0.1"]')
    pg.wait_for_timeout(300)
    pg.click(trim + ' button[data-a="start"]')
    pg.wait_for_timeout(300)
    check("⇤ Start here", pg.inner_text(trim + " .trim-start") == "0:01.1", pg.inner_text(trim + " .trim-start"))
    pg.click(trim + ' button[data-a="+1"]')
    pg.click(trim + ' button[data-a="-0.1"]')
    pg.wait_for_timeout(300)
    pg.click(trim + ' button[data-a="end"]')
    pg.wait_for_timeout(300)
    check("End here ⇥", pg.inner_text(trim + " .trim-end") == "0:02.0" and part() == "0.9 s chosen", (pg.inner_text(trim + " .trim-end"), part()))
    pg.click(trim + ' button[data-a="-1"]')
    pg.click(trim + ' button[data-a="gostart"]')
    pg.wait_for_timeout(300)
    check("Go to part start", abs(vt() - 1.1) < 0.05, vt())
    pg.click(trim + ' button[data-a="goend"]')
    pg.wait_for_timeout(300)
    check("Go to part end (its last frame)", abs(vt() - 1.9) < 0.05, vt())
    for fps in ["5", "15", "10"]:
        pg.click(trim + ' button[data-fps="%s"]' % fps)
        pg.wait_for_timeout(200)
    check("fps 5 / 15 / 10", "active" in pg.get_attribute(trim + ' button[data-fps="10"]', "class"))
    pg.click(trim + ' button[data-a="loop"]')
    pg.wait_for_timeout(1500)
    ts = [vt() for _ in range(8) if not pg.wait_for_timeout(150)]
    check("▶ Play the part: stays inside the part", all(1.0 <= t <= 2.15 for t in ts), ts)
    pg.click(trim + ' button[data-a="play"]')
    pg.click(trim + ' button[data-a="play"]')
    pg.fill(trim + " .trim-time", "0:03")
    pg.press(trim + " .trim-time", "Enter")
    press_range(pg, trim + " .trim-bar input", ["ArrowLeft"])
    pg.click(trim + ' button[data-a="gostart"]')
    pg.wait_for_timeout(2500)
    check("the part read: an animation of its frames", pg.evaluate("bi.frames ? bi.frames.length : 0") == 9 and not pg.is_disabled("#bi-save"),
          (pg.evaluate("bi.frames ? bi.frames.length : 0"), pg.inner_text("#bi-size")))
    pg.click("#bi-save")
    pg.wait_for_timeout(2000)
    p = builtin("Torah")
    check("... saved as that animation", p.get("custom") and p.get("customFrames") == 9, p)
    for name, label in [("Havdala", "Havdalah"), ("Torah", "Parasha")]:
        http(HOME, "/api/builtin/reset?name=" + name, form={"body": "{}"})
    check("cleaned up", not builtin("Havdala").get("custom") and not builtin("Torah").get("custom"))

def sim_state():
    try:
        return json.loads(ctl("/state") or b"{}")
    except ValueError:
        return {}


def world():
    return json.loads(ctl("/world"))


def set_world(w):
    ctl("/world", json.dumps(w).encode())


def row_menu(pg, n, button_id):
    """⋯ of network n, then one of its menu's buttons."""
    pg.locator("#wifi-networks .rowmenu-btn").nth(n - 1).click()
    pg.wait_for_timeout(200)
    pg.click("#" + button_id)
    pg.wait_for_timeout(300)


def wait_scan(pg, n, timeout=25):
    return wait(lambda: not pg.is_disabled("#cfg-ssid-pick-%d" % n) and pg.locator("#cfg-ssid-pick-%d option" % n).count() > 1, timeout, 0.5)


@section
def wifi(ui, clock):
    """Wi-Fi: clock name, 24-hour, each network's menu (Connect, Add/Change network, Remove), the network list (every choice, ↻, Other…), password Show/Hide, Save Wi-Fi."""
    pg = ui.page
    w = world()
    w["networks"].append({"ssid": "UIT-Net", "password": "uit-pass-1", "ip": "192.168.77.240", "internet": True, "up": True,
                          "rssi": -50, "channel": 11, "bssid": "5A:1E:00:00:77:01"})
    set_world(w)
    ui.goto(HOME + "/")
    ui.settings()
    c0 = cfg()
    name0 = c0["projectName"]
    # name
    pg.fill("#cfg-name", "test clock")
    said = ui.click_msg("button:has-text('Save Wi-Fi')")
    check("Clock name saved (in the clock's form)", cfg()["projectName"] == "TestClock", (said, cfg()["projectName"]))
    check("... the top bar shows it", wait(lambda: pg.inner_text("#brand-name") == "Test Clock", 5), pg.inner_text("#brand-name"))
    pg.click("#cfg-24h + .sl")
    check("24-hour clock switched", wait_cfg("use24Hour", not c0["use24Hour"]))
    pg.click("#cfg-24h + .sl")
    check("... and back", wait_cfg("use24Hour", c0["use24Hour"]))
    # Network 3 (empty): Add network
    check("an empty network: (empty), no Connect / Remove / place", pg.inner_text("#cfg-ssid-display-3") == "(empty)")
    row_menu(pg, 3, "cfg-wifi-change-3")
    check("Add network opens its fields", pg.is_visible("#cfg-ssid-pick-3"))
    check("the list of networks the clock hears", wait_scan(pg, 3))
    opts = pg.eval_on_selector_all("#cfg-ssid-pick-3 option", "os => os.map(o => o.textContent)")
    check("... every network around, strongest first, saved ones marked, Other… last",
          "UIT-Net" in opts[1] and any("saved" in o for o in opts) and opts[-1] == "Other…", opts)
    for i, v in pg.eval_on_selector_all("#cfg-ssid-pick-3 option", "os => os.map((o, i) => [i, o.value, o.disabled]).filter(x => !x[2]).map(x => [x[0], x[1]])"):
        pg.select_option("#cfg-ssid-pick-3", index=i)
        if v:
            check("choosing %s fills the name" % v, pg.input_value("#cfg-ssid-3") == v and not pg.is_visible("#cfg-ssid-3"))
    pg.select_option("#cfg-ssid-pick-3", label="Other…")
    check("Other…: a box to type a name", pg.is_visible("#cfg-ssid-3"))
    pg.fill("#cfg-ssid-3", "UIT-Net")
    pg.click("#cfg-ssid-rescan-3")
    check("↻ searches again", wait_scan(pg, 3))
    pg.select_option("#cfg-ssid-pick-3", label="Other…")
    pg.fill("#cfg-ssid-3", "UIT-Net")
    pg.fill("#cfg-pass-3", "uit-pass-1")
    pg.click("#cfg-pass-show-3")
    check("Show: the password readable", pg.get_attribute("#cfg-pass-3", "type") == "text" and pg.inner_text("#cfg-pass-show-3") == "Hide")
    pg.click("#cfg-pass-show-3")
    check("Hide: hidden again", pg.get_attribute("#cfg-pass-3", "type") == "password")
    said = ui.click_msg("button:has-text('Save Wi-Fi')")
    c = cfg()
    check("Save Wi-Fi: network 3 saved", c.get("wifiSSID3") == "UIT-Net", said)
    pg.wait_for_timeout(800)
    check("... shown as network 3, its fields closed", pg.inner_text("#cfg-ssid-display-3") == "UIT-Net" and not pg.is_visible("#cfg-ssid-pick-3"))
    # Every network: Connect (the clock moves there), its fields open/close, Show/Hide, ↻
    names = {1: c["wifiSSID"], 2: c.get("wifiSSID2", ""), 3: "UIT-Net"}
    for n in (3, 2, 1):
        if not names[n]:
            check("network %d is set in the test clock's settings" % n, False)
            continue
        if sim_state().get("ssid") == names[n]:   # already there: Connect is hidden for it - go elsewhere first
            other = 3 if n != 3 else 2
            ui.dialog_answer[0] = True
            row_menu(pg, other, "cfg-wifi-connect-%d" % other)
            wait(lambda: sim_state().get("ssid") == names[other] and up(), 60, 1)
            pg.wait_for_timeout(2000)
        ui.dialog_answer[0] = False
        n0 = len(ui.dialogs)
        row_menu(pg, n, "cfg-wifi-connect-%d" % n)
        check("Connect %d asks first (No: stays)" % n, any(names[n] in m for _, m in ui.dialogs[n0:]) and sim_state().get("ssid") != names[n])
        ui.dialog_answer[0] = True
        row_menu(pg, n, "cfg-wifi-connect-%d" % n)
        check("Connect %d: the clock moves to %s" % (n, names[n]), wait(lambda: sim_state().get("ssid") == names[n] and up(), 60, 1), sim_state().get("ssid"))
        check("... the page marks it connected, no Connect for it", wait(lambda: "wifi-connected" in (pg.get_attribute("#cfg-ssid-display-%d" % n, "class") or ""), 10)
              and pg.locator("#cfg-wifi-connect-%d.hidden" % n).count() == 1)
        # its place: Detect, Confirm place, then a city searched (Search button / Enter) and chosen
        row_menu(pg, n, "place-change-%d" % n)
        check("Change place %d: a city box, Detect (the network it's on)" % n, pg.is_visible("#place-q-%d" % n) and pg.is_visible("#place-detect-%d" % n))
        pg.click("#place-detect-%d" % n)
        check("Detect %d: looked up, not confirmed" % n, wait(lambda: cfg()["places"][n - 1].get("set") and cfg()["places"][n - 1].get("confirmed") is False, 40, 1),
              cfg()["places"][n - 1])
        check("... the row says detected", wait(lambda: pg.is_visible("#place-detected-%d" % n), 15))
        row_menu(pg, n, "place-ok-%d" % n)
        check("Confirm place %d" % n, wait(lambda: cfg()["places"][n - 1].get("confirmed") is True, 10) and wait(lambda: not pg.is_visible("#place-detected-%d" % n), 8))
        row_menu(pg, n, "place-change-%d" % n)
        city = {1: "Haifa", 2: "Jerusalem", 3: "London"}[n]
        pg.fill("#place-q-%d" % n, city)
        if n == 2:
            pg.press("#place-q-2", "Enter")
        else:
            pg.click("#place-edit-%d button:has-text('Search')" % n)
        hits = pg.locator("#place-results-%d .place-hit" % n)
        check("search %s finds it" % city, wait(lambda: hits.count() > 0, 30, 0.5), pg.inner_text("#place-results-%d" % n))
        hits.first.click()
        check("a result chosen: network %d's place, confirmed" % n, wait(lambda: cfg()["places"][n - 1].get("city") == city and cfg()["places"][n - 1].get("confirmed") is True, 10),
              cfg()["places"][n - 1])
        check("... shown in its row", wait(lambda: city in pg.inner_text("#place-txt-%d" % n), 8), pg.inner_text("#place-txt-%d" % n))
        if n == 3:
            check("... and the clock (on that network) runs on its time zone", wait(lambda: "London" in json.dumps(status()) or status().get("tzRule", "").startswith("GMT"), 15), status().get("tzRule"))
            row_menu(pg, 3, "place-change-3")
            pg.fill("#place-q-3", "Qqxxzzvvyy")
            pg.press("#place-q-3", "Enter")
            check("a city that doesn't exist: Not found", wait(lambda: pg.inner_text("#place-results-3") == "Not found", 30, 0.5), pg.inner_text("#place-results-3"))
            row_menu(pg, 3, "place-change-3")
            check("Change place again closes the box", not pg.is_visible("#place-q-3"))
        if n != 3:
            row_menu(pg, n, "cfg-wifi-change-%d" % n)
            check("Change network %d opens its fields" % n, pg.is_visible("#cfg-ssid-pick-%d" % n) and wait_scan(pg, n))
            for i in pg.eval_on_selector_all("#cfg-ssid-pick-%d option" % n, "os => os.map((o, i) => o.disabled ? -1 : i).filter(i => i >= 0)"):
                pg.select_option("#cfg-ssid-pick-%d" % n, index=i)
            pg.click("#cfg-ssid-rescan-%d" % n)
            wait_scan(pg, n)
            pg.click("#cfg-pass-show-%d" % n)
            pg.fill("#cfg-pass-%d" % n, "x")
            pg.fill("#cfg-pass-%d" % n, "")
            pg.click("#cfg-pass-show-%d" % n)
            row_menu(pg, n, "cfg-wifi-change-%d" % n)
            check("... and closes them again (nothing saved)", not pg.is_visible("#cfg-ssid-pick-%d" % n) and cfg().get("wifiSSID%s" % ("" if n == 1 else n)) == names[n])
    # The ⋯ menu closes when clicking elsewhere
    pg.locator("#wifi-networks .rowmenu-btn").nth(0).click()
    pg.locator("#wifi-networks .rowmenu-btn").nth(0).click()
    check("⋯ twice: the menu closes", pg.locator("#wifi-networks .rowmenu:not(.hidden)").count() == 0)
    pg.locator("#wifi-networks .rowmenu-btn").nth(0).click()
    pg.click("#cfg-name")
    check("a click elsewhere closes the menu", pg.locator("#wifi-networks .rowmenu:not(.hidden)").count() == 0)
    # Remove network 3 (No, then Yes)
    ui.dialog_answer[0] = False
    row_menu(pg, 3, "cfg-wifi-remove-3")
    check("Remove asks first (No: kept)", cfg().get("wifiSSID3") == "UIT-Net")
    ui.dialog_answer[0] = True
    row_menu(pg, 3, "cfg-wifi-remove-3")
    pg.wait_for_timeout(800)
    check("Remove: network 3 gone", cfg().get("wifiSSID3", "") == "" and pg.inner_text("#cfg-ssid-display-3") == "(empty)")
    # Networks 2 and 1: Remove, then put back by hand (Other…, the name and password typed)
    pw = {n["ssid"]: n["password"] for n in world()["networks"]}
    for n in (2, 1):
        key = "wifiSSID" + ("" if n == 1 else str(n))
        ui.dialog_answer[0] = True
        row_menu(pg, n, "cfg-wifi-remove-%d" % n)
        pg.wait_for_timeout(800)
        check("Remove network %d" % n, cfg().get(key, "") == "" and pg.inner_text("#cfg-ssid-display-%d" % n) == "(empty)")
        row_menu(pg, n, "cfg-wifi-change-%d" % n)
        wait_scan(pg, n)
        pg.select_option("#cfg-ssid-pick-%d" % n, label="Other…")
        pg.fill("#cfg-ssid-%d" % n, names[n])
        pg.fill("#cfg-pass-%d" % n, pw[names[n]])
        pg.click("button:has-text('Save Wi-Fi')")
        check("... put back by typing it (Other…)", wait(lambda: cfg().get(key) == names[n], 6) and wait(lambda: pg.inner_text("#cfg-ssid-display-%d" % n) == names[n], 6))
    # name back
    pg.fill("#cfg-name", name0)
    pg.click("button:has-text('Save Wi-Fi')")
    check("name back", wait(lambda: cfg()["projectName"] == name0, 5))

def download(pg, selector, timeout=30000):
    """Click; the file the page saves (path), or None."""
    try:
        with pg.expect_download(timeout=timeout) as d:
            pg.click(selector)
        path = OUT / "files" / d.value.suggested_filename
        d.value.save_as(str(path))
        return path
    except Exception as e:
        log("no download: %s" % e)
        return None


def xlsx_sheets(path):
    import zipfile
    z = zipfile.ZipFile(path)
    return {n: z.read(n).decode("utf-8") for n in z.namelist() if n.startswith("xl/worksheets/") and n.endswith(".xml")}


def xlsx_edit(src, dst, fn):
    """A copy of an .xlsx with fn(name, xml) -> xml applied to each worksheet."""
    import zipfile
    zi = zipfile.ZipFile(src)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
        for item in zi.infolist():
            data = zi.read(item.filename)
            if item.filename.startswith("xl/worksheets/") and item.filename.endswith(".xml"):
                data = fn(item.filename, data.decode("utf-8")).encode("utf-8")
            zo.writestr(item, data)
    return str(dst)


def events_sheet(sheets):
    return next(x for n, x in sheets.items() if "UIT Imp" in x or "settings.xml" not in n and "<sheetData" in x and "Event" in x)


def import_file(pg, path):
    pg.evaluate("closeDataBox()")
    with pg.expect_file_chooser() as fc:
        pg.click("button[onclick=\"el('data-upload-input').click()\"]")
    fc.value.set_files(path)
    wait(lambda: pg.is_visible("#data-result"), 10, 0.2)
    pg.wait_for_timeout(300)
    return pg.inner_text("#data-result") if pg.is_visible("#data-result") else ""


def data_button(pg, label):
    pg.locator("#data-result button", has_text=label).click()
    pg.wait_for_timeout(1200)


@section
def export_import(ui, clock):
    """Export & import: Events / Settings / Both, a changed file imported (Cancel, Import, Undo, Close), a damaged one, settings and both (with the restart)."""
    import re
    pg = ui.page
    remove_test_events()
    add_event(name="UIT Imp A", hasG=True, gD=3, gM=5, gY=1980, type="birthday")
    add_event(name="UIT Imp B", hasJ=True, jD=7, jMonth="אב", jYNum=5740, jY="תש״מ", type="yahrzeit")
    # dates that don't match (14 Mar 1990 is 17 Adar, not 20): only an import can store it
    store_events(dict(name="UIT Imp C", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=20, jMonth="אדר", jYNum=5750, jY="תש״נ", type="other"))
    ui.goto(HOME + "/")
    ui.settings()
    pg.click("button[onclick='downloadData()']")
    pg.wait_for_timeout(500)
    check("Export before choosing: says to choose first", "Choose Events, Settings or Both first" in pg.inner_text("#data-result"))
    data_button(pg, "Close")
    check("Close closes it", not pg.is_visible("#data-result"))
    n_events = len(events())
    files = {}
    for scope in ("events", "settings", "both"):
        pg.click("#scope-" + scope)
        check("%s chosen (its line explains it)" % scope, "active" in pg.get_attribute("#scope-" + scope, "class") and pg.inner_text("#data-hint") != "")
        files[scope] = download(pg, "button[onclick='downloadData()']")
        check("Export %s: a file" % scope, files[scope] and files[scope].name.startswith("JewishClock-" + {"events": "events", "settings": "settings", "both": "backup"}[scope]), files[scope])
    sh = {k: xlsx_sheets(v) for k, v in files.items() if v}
    ev_xml = lambda s: next((x for n, x in s.items() if "settings.xml" not in n and "UIT Imp A" in x), "")
    check("events file: every event, no settings", ev_xml(sh["events"]).count("<row ") - 1 == n_events and not any("settings.xml" in n for n in sh["events"]),
          ev_xml(sh["events"]).count("<row "))
    check("settings file: the settings, no events", any("settings.xml" in n and "projectName" in x for n, x in sh["settings"].items()) and ev_xml(sh["settings"]) == "")
    check("both: events and settings", ev_xml(sh["both"]) != "" and any("settings.xml" in n for n in sh["both"]))
    # A changed events file: A renamed, B dropped -> 1 new, 1 removed ... Cancel / Import / Undo
    def change(name, x):
        if "UIT Imp A" not in x:
            return x
        x = x.replace(">UIT Imp A<", ">UIT Imp A2<")
        return re.sub(r'<row r="\d+">(?:(?!</row>).)*UIT Imp B(?:(?!</row>).)*</row>', "", x, flags=re.S)
    changed = xlsx_edit(files["events"], OUT / "files" / "changed.xlsx", change)
    said = import_file(pg, changed)
    check("a file: says what is in it, asks what to import", "%d event(s)" % (n_events - 1) in said and pg.locator("#data-result button", has_text="Import events").count() == 1, said[:200])
    data_button(pg, "Cancel")
    check("Cancel: nothing changed", find_event("UIT Imp A") and find_event("UIT Imp B") and not find_event("UIT Imp A2"))
    import_file(pg, changed)
    data_button(pg, "Import events")
    said = pg.inner_text("#data-result")
    check("Import events: says what changed (a rename = 1 new + 1 removed)", "1 new" in said and "2 removed" in said and "UIT Imp A2" in said, said[:300])
    check("... and the clock has it so", find_event("UIT Imp A2") and not find_event("UIT Imp A") and not find_event("UIT Imp B"))
    row_c = re.search(r'<row r="(\d+)">(?:(?!</row>).)*UIT Imp C', ev_xml(xlsx_sheets(changed)) or next(x for x in xlsx_sheets(changed).values() if "UIT Imp C" in x), re.S)
    check("... a row whose dates don't match: flagged with its row, both dates and the right one",
          row_c and ("Row %s (UIT Imp C): Date mismatch: 14/3/1990 is" % row_c.group(1)) in said and "י״ז אדר" in said and "but the Hebrew date says כ׳ אדר" in said,
          said[said.find("check these"):][:300])
    data_button(pg, "Undo this import")
    check("Undo: the events as before", find_event("UIT Imp A") and find_event("UIT Imp B") and not find_event("UIT Imp A2"), pg.inner_text("#data-result")[:200])
    data_button(pg, "Close")
    # A damaged file / not an Excel file
    bad = xlsx_edit(files["events"], OUT / "files" / "bad.xlsx", lambda n, x: x.replace(">yahrzeit<", ">bogus<"))
    said = import_file(pg, bad)
    check("a file with a bad row: nothing can be imported, says which row", "not one of birthday" in said and pg.locator("#data-result button", has_text="Import events").count() == 0, said[:200])
    data_button(pg, "Cancel")
    said = import_file(pg, f("notapic.txt"))
    check("not an Excel file: Nothing was changed", "Nothing was changed" in said and "not a file exported from this clock" in said, said[:200])
    data_button(pg, "Close")
    # Settings: a changed setting comes in; the clock restarts, the page reloads by itself
    def set_scroll(name, x):
        return re.sub(r'(<t xml:space="preserve">scrollSpeed</t></is></c><c r="B\d+"><v>)\d+(</v>)', r"\g<1>7\2", x)
    sfile = xlsx_edit(files["settings"], OUT / "files" / "settings7.xlsx", set_scroll)
    said = import_file(pg, sfile)
    check("a settings file: names its Wi-Fi, offers Import settings", "settings" in said and "Wi-Fi" in said and pg.locator("#data-result button", has_text="Import settings").count() == 1, said[:200])
    up0 = status()["uptimeMs"]
    data_button(pg, "Import settings")
    check("Import settings: the clock restarts", wait(lambda: status()["uptimeMs"] < up0, 90, 1))
    check("... with the imported setting", wait(lambda: cfg()["scrollSpeed"] == 7, 60, 1), cfg().get("scrollSpeed"))
    check("... and the page reloads by itself", wait(lambda: pg.evaluate("performance.now()") < 60000 and pg.is_visible("#view-calendar"), 90, 1))
    wait(synced, 90, 1)
    # Both: the backup as exported (events + settings) puts everything back
    ui.settings()
    said = import_file(pg, files["both"])
    check("a backup: offers Import both", pg.locator("#data-result button", has_text="Import both").count() == 1, said[:200])
    data_button(pg, "Import both")
    check("Import both: settings as in the backup", wait(lambda: cfg()["scrollSpeed"] != 7, 90, 1), cfg().get("scrollSpeed"))
    check("... events as in the backup", wait(lambda: find_event("UIT Imp A") and find_event("UIT Imp B") and len(events()) == n_events, 30, 1))
    wait(synced, 90, 1)
    pg.wait_for_timeout(3000)
    remove_test_events()


@section
def clock_software(ui, clock):
    """Clock software: version, Quick start guide, Manual, Licenses, Update the clock, Get log."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    v = status()["firmware"]
    check("the version shown", v in pg.inner_text("#fwupd-version"), pg.inner_text("#fwupd-version"))
    for label, path, want in [("Quick start guide", "/start.html", "For clock software version"), ("Manual", "/manual.html", "For clock software version"),
                              ("Licenses", "/licenses.txt", "LICENSE")]:
        with ui.ctx.expect_page() as np:
            pg.click("a:has-text('%s')" % label)
        p2 = np.value
        p2.wait_for_load_state()
        check("%s opens in a new tab" % label, p2.url.endswith(path) and want.lower() in p2.content().lower(), p2.url)
        harvest(p2)
        p2.close()
    path = download(pg, "button:has-text('Get log')")
    text = path.read_text(encoding="utf-8") if path else ""
    check("Get log: a text file with status, settings (no passwords) and the log",
          "== Status ==" in text and "== Settings ==" in text and "== Diagnostic log ==" in text and "uit-pass-1" not in text, path)
    pg.click("a:has-text('Update the clock')")
    pg.wait_for_load_state()
    check("Update the clock: the update page", pg.url.endswith("/update.html"), pg.url)

def clock_local():
    """The clock's local time now (its debug offset included), as a datetime."""
    s = status()
    return datetime.datetime.utcfromtimestamp(s["utcTime"] + s.get("utcOffsetSec", 0))


@section
def debug_time(ui, clock):
    """Debug time: Shift by d/h/min, Set to (Gregorian), Set to (Hebrew) - every month, letters/digits/short years, wrong dates - Back to real time; the screen follows."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    real = clock_local()
    today = datetime.date.today()
    check("the date fields start at today", pg.input_value("#dbg-g")[:10] == today.isoformat() and pg.input_value("#dbg-hy") != "" and pg.input_value("#dbg-hd") != "",
          (pg.input_value("#dbg-g"), pg.input_value("#dbg-hd"), pg.input_value("#dbg-hm"), pg.input_value("#dbg-hy")))
    # Shift
    pg.fill("#dbg-d", "3")
    pg.fill("#dbg-h", "2")
    pg.fill("#dbg-m", "30")
    said = ui.click_msg("button[onclick='debugShift()']")
    want = 3 * 86400 + 2 * 3600 + 30 * 60
    check("Shift by 3 d 2 h 30 min", cfg()["debugTimeOffset"] == want and "The clock now thinks" in said, (cfg()["debugTimeOffset"], said))
    d = (clock_local() - real).total_seconds()
    check("... the clock's time moved so", abs(d - want) < 120, d)
    check("... the page says so", "+3 d" in pg.inner_text("#dbg-state") or "3 d" in pg.inner_text("#dbg-state") or pg.inner_text("#dbg-state") != "Real time", pg.inner_text("#dbg-state"))
    # Gregorian
    pg.fill("#dbg-g", "2027-04-23T21:30")
    ui.click_msg("button[onclick='debugSetGregorian()']")
    check("Set to (Gregorian) 23 Apr 2027 21:30", wait(lambda: clock_local().strftime("%Y-%m-%d %H:%M") in ("2027-04-23 21:30", "2027-04-23 21:31"), 10), clock_local())
    pg.fill("#dbg-g", "")
    check("Gregorian empty: says to choose one", "Choose a date" in ui.click_msg("button[onclick='debugSetGregorian()']"))
    # Hebrew: every month of 5787 (a leap year: Adar = Adar I, and Adar II), day 10, at 12:00
    pg.fill("#dbg-ht", "12:00")
    bad = []
    for m in H_MONTHS + ["אדר ב׳"]:
        pg.fill("#dbg-hd", "10")
        pg.select_option("#dbg-hm", m)
        pg.fill("#dbg-hy", "תשפ״ז")
        said = ui.click_msg("button[onclick='debugSetHebrew()']")
        want_m = {"אדר": "אדר א׳"}.get(m, m)
        got = wait(lambda: (lambda h: h if h.startswith("י׳ " + want_m) else None)(status()["hebrewDate"]), 10)
        if not got:
            bad.append((m, status()["hebrewDate"], said))
    check("Set to (Hebrew): 10 of every month of 5787 (Adar I and II too)", not bad, bad)
    for day, year, want_h in [("ט״ו", "5787", "ט״ו ניסן תשפ״ז"), ("15", "787", "ט״ו ניסן תשפ״ז"), ("15", "פז", "ט״ו ניסן תשפ״ז")]:
        pg.fill("#dbg-hd", day)
        pg.select_option("#dbg-hm", "ניסן")
        pg.fill("#dbg-hy", year)
        ui.click_msg("button[onclick='debugSetHebrew()']")
        check("Hebrew typed as %s / %s" % (day, year), wait(lambda: status()["hebrewDate"] == want_h, 10), status()["hebrewDate"])
    pg.fill("#dbg-hd", "30")
    pg.select_option("#dbg-hm", "אדר ב׳")
    pg.fill("#dbg-hy", "תשפ״ז")
    said = ui.click_msg("button[onclick='debugSetHebrew()']")
    check("a day the month doesn't have: refused, the clock stays", "There is no" in said and status()["hebrewDate"] == "ט״ו ניסן תשפ״ז", said)
    pg.fill("#dbg-hd", "")
    check("no day: says what to type", "Hebrew day" in ui.click_msg("button[onclick='debugSetHebrew()']"))
    pg.fill("#dbg-hd", "5")
    pg.fill("#dbg-hy", "")
    check("no year: says what to type", "Hebrew year" in ui.click_msg("button[onclick='debugSetHebrew()']"))
    pg.fill("#dbg-hd", "5"); pg.select_option("#dbg-hm", "ניסן"); pg.fill("#dbg-hy", "2027")
    pg.click("button[onclick='debugSetHebrew()']")
    pg.wait_for_timeout(800)
    check("a Gregorian year in the Hebrew box: 'Did you mean תשפ״ז?'", modal_open(pg, "modal-confirm") and "Did you mean תשפ״ז" in pg.inner_text("#cf-title"), pg.inner_text("#cf-title"))
    ui.clear_toast()
    ui.ask_answer("Cancel")
    check("... Cancel: says why, nothing set", "looks like a Gregorian year" in ui.wait_toast())
    pg.click("button[onclick='debugSetHebrew()']")
    pg.wait_for_timeout(800)
    ui.ask_answer("Yes - תשפ״ז")
    check("... Yes: put in, and the clock goes there", pg.input_value("#dbg-hy") == "תשפ״ז" and wait(lambda: status()["hebrewDate"] == "ה׳ ניסן תשפ״ז", 10), status()["hebrewDate"])
    pg.fill("#dbg-hd", "מד")
    check("no such Hebrew day (מד): says so", "is not a Hebrew day" in ui.click_msg("button[onclick='debugSetHebrew()']"))
    dspin = lambda field, d: pg.click(".hspin:has(#%s) button[data-step='%d']" % (field, d))
    pg.fill("#dbg-hd", "ט״ו"); pg.fill("#dbg-hy", "תשפ״ז")
    dspin("dbg-hd", 1); dspin("dbg-hy", 1)
    check("Debug Hebrew ▴: ט״ז, תשפ״ח", (pg.input_value("#dbg-hd"), pg.input_value("#dbg-hy")) == ("ט״ז", "תשפ״ח"),
          (pg.input_value("#dbg-hd"), pg.input_value("#dbg-hy")))
    dspin("dbg-hd", -1); dspin("dbg-hy", -1)
    check("... ▾: ט״ו, תשפ״ז", (pg.input_value("#dbg-hd"), pg.input_value("#dbg-hy")) == ("ט״ו", "תשפ״ז"),
          (pg.input_value("#dbg-hd"), pg.input_value("#dbg-hy")))
    # The screen follows: Omer day 23 (8 Iyar 5787 = 25 Apr 2027 daytime) in the compact wording
    api_post("/api/config", {"omerMode": 4, "rotationMode": 0, "messageDwellSec": 2})
    pg.fill("#dbg-hd", "8")
    pg.select_option("#dbg-hm", "אייר")
    pg.fill("#dbg-hy", "תשפ״ז")
    ui.click_msg("button[onclick='debugSetHebrew()']")
    shown, seen = screen_shows(lambda t: "שבועות" in t and "23" in t, 120)
    check("the clock's screen at that date: Omer 23, compact", shown is not None and "=" in shown, (shown, seen[-12:]))
    api_post("/api/config", {"omerMode": 1})
    # Back to real time
    said = ui.click_msg("button[onclick*='debugTime']")
    check("Back to real time", cfg()["debugTimeOffset"] == 0 and abs((clock_local() - real).total_seconds()) < 600 and "Back to real time" in said, said)


@section
def limits(ui, clock):
    """Limits: the page's sliders and boxes take the clock's (/api/limits); the clock keeps every value inside them."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    pg.wait_for_timeout(1500)
    lim = jget("/api/limits")
    # every box / slider with a limit: its min/max are the clock's
    got = pg.evaluate("""() => [...document.querySelectorAll('[data-limit]')].map(i => [i.id || i.parentNode.id, i.dataset.limit, +i.min, +i.max, +(i.dataset.limitScale || 1)])""")
    bad = [g for g in got if g[1] not in lim["ranges"] or abs(g[2] - lim["ranges"][g[1]][0] * g[4]) > 1e-9 or abs(g[3] - lim["ranges"][g[1]][1] * g[4]) > 1e-9]
    check("every slider and box with a limit: min/max as the clock says (%d of them)" % len(got), len(got) >= 12 and not bad, bad)
    y0, y1 = lim["ranges"]["debugYear"]
    check("Debug time's date: the clock's years", pg.get_attribute("#dbg-g", "min").startswith(str(y0)) and pg.get_attribute("#dbg-g", "max").startswith(str(y1)))
    # what the page still keeps itself must agree with the clock
    check("the page's Wi-Fi slots: as many as the clock keeps", pg.evaluate("WIFI_SLOTS") == lim["wifiNetworks"], (pg.evaluate("WIFI_SLOTS"), lim["wifiNetworks"]))
    check("the page's Hebrew months: the clock's names", pg.evaluate("H_MONTHS") == lim["hebrewMonths"][:12], (pg.evaluate("H_MONTHS"), lim["hebrewMonths"]))
    check("the event type list: the clock's types", pg.eval_on_selector_all("#ev-type option", "os => os.map(o => o.value)") == [t["type"] for t in lim["eventTypes"]])
    # the clock keeps values inside its limits, whoever sends them
    c0 = cfg()
    api_post("/api/config", {"scrollSpeed": 99, "messageDwellSec": 0, "calibTearUs": 99999, "brightPct": -5})
    c = cfg()
    check("values beyond the limits: the clock keeps them inside", (c["scrollSpeed"], c["messageDwellSec"], c["calibTearUs"], c["brightPct"]) ==
          (lim["ranges"]["scrollSpeed"][1], lim["ranges"]["messageDwellSec"][0], lim["ranges"]["calibTearUs"][1], lim["ranges"]["brightPct"][0]),
          (c["scrollSpeed"], c["messageDwellSec"], c["calibTearUs"], c["brightPct"]))
    api_post("/api/config", {"calibX": 300, "calibW": 400})
    c = cfg()
    check("a window past the screen's edge: its size kept, moved back on", c["calibW"] == 400 and c["calibX"] + c["calibW"] <= lim["screenW"], (c["calibX"], c["calibW"]))
    api_post("/api/config", {k: c0[k] for k in ("scrollSpeed", "messageDwellSec", "calibTearUs", "brightPct", "calibX", "calibW")})
    check("... and back as they were", all(cfg()[k] == c0[k] for k in ("scrollSpeed", "messageDwellSec", "calibTearUs", "brightPct", "calibX", "calibW")))
    # events
    w1 = lim["ranges"]["warn"][1]
    i = add_event(name="UIT Limit", hasG=True, gD=1, gM=1, gY=0, warn=w1 * 10)
    e = find_event("UIT Limit")
    check("an event reminding %d days before: the clock keeps %d" % (w1 * 10, w1), e and e["warn"] == w1, e and e["warn"])
    code, b = api_post("/api/events/upsert", {"name": "UIT Limit2", "hasG": True, "gD": 1, "gM": 1, "gY": lim["ranges"]["gYear"][0] - 1, "hasJ": False,
                                               "jD": 0, "jMonth": "", "jYNum": 0, "type": "other", "icon": "", "warn": 0, "image": ""})
    check("a year before %d: refused, says the range" % lim["ranges"]["gYear"][0], code == 400 and str(lim["ranges"]["gYear"][0]) in b.decode(), (code, b[:120]))
    ui.calendar()
    pg.click("button.fab")
    pg.wait_for_timeout(1000)
    pg.fill("#ev-name", "UIT Limit3")
    pg.fill("#ev-warn", str(w1 + 1))
    said = ui.click_msg("#modal-event button:has-text('Save')")
    check("the dialog: Remind %d days refused, says the most" % (w1 + 1), "at most %d days" % w1 in said and modal_open(pg) and not find_event("UIT Limit3"), said)
    pg.locator("#modal-event button", has_text="Cancel").click()
    remove_test_events()


@section
def advanced(ui, clock):
    """Advanced: NTP server, window calibration (4 numbers, Tear lines, Off/Content/Pattern + live picture), Demo, Freeze screen, Hebcal data, log level, the log."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    c0 = cfg()
    pg.fill("#cfg-ntp0", "time.google.com")
    pg.press("#cfg-ntp0", "Tab")
    check("NTP server", wait(lambda: cfg()["ntpServers"][0] == "time.google.com", 6), cfg()["ntpServers"])
    pg.fill("#cfg-ntp0", c0["ntpServers"][0])
    pg.press("#cfg-ntp0", "Tab")
    wait(lambda: cfg()["ntpServers"][0] == c0["ntpServers"][0], 6)
    # One step each way that keeps the window on the screen (the clock moves a window past
    # the edge back on - the limits section checks that): the default window ends at the
    # screen's bottom edge, so Y and H go down, not up.
    lim = jget("/api/limits")
    room = {"calibX": lim["screenW"] - c0["calibX"] - c0["calibW"], "calibW": lim["screenW"] - c0["calibX"] - c0["calibW"],
            "calibY": lim["screenH"] - c0["calibY"] - c0["calibH"], "calibH": lim["screenH"] - c0["calibY"] - c0["calibH"]}
    for sel, key in [("#cfg-cal-x", "calibX"), ("#cfg-cal-y", "calibY"), ("#cfg-cal-w", "calibW"), ("#cfg-cal-h", "calibH")]:
        step_ = 1 if room[key] > 0 else -1
        if c0[key] + step_ < lim["ranges"][key][0]:
            step_ = 1
        pg.fill(sel, str(c0[key] + step_))
        check("calibration %s (%+d)" % (key, step_), wait_cfg(key, c0[key] + step_))
        pg.fill(sel, str(c0[key]))
        wait_cfg(key, c0[key])
    check("... the window as it was", all(cfg()[k] == c0[k] for k in ("calibX", "calibY", "calibW", "calibH")),
          [(k, cfg()[k], c0[k]) for k in ("calibX", "calibY", "calibW", "calibH")])
    press_range(pg, "#cfg-cal-tear-s", ["ArrowRight"] * 3)
    check("Tear lines", wait(lambda: cfg()["calibTearUs"] == round(c0["calibTearUs"] / 100) * 100 + 300, 6) and pg.inner_text("#cfg-cal-tear-lbl") == "%.1f" % ((round(c0["calibTearUs"] / 100) + 3) / 10),
          (cfg()["calibTearUs"], pg.inner_text("#cfg-cal-tear-lbl")))
    press_range(pg, "#cfg-cal-tear-s", ["ArrowLeft"] * 3)
    for mode in ("content", "pattern", "off"):
        pg.click("#cal-" + mode)
        pg.wait_for_timeout(2500)
        mirror = pg.is_visible("#cal-mirror") and pg.evaluate("el('cal-mirror').naturalWidth") > 0
        check("Calibrate %s (the live picture of the screen %s)" % (mode, "shows" if mode != "off" else "hides"), mirror == (mode != "off") and "active" in pg.get_attribute("#cal-" + mode, "class"))
    # Demo
    pg.click("#demo-btn")
    check("Start demo: the clock runs it, the button says Stop", wait(lambda: status()["demo"] is True, 5) and wait(lambda: pg.inner_text("#demo-btn") == "Stop demo", 5))
    pg.click("#demo-btn")
    check("Stop demo", wait(lambda: status()["demo"] is False, 5) and wait(lambda: pg.inner_text("#demo-btn") == "Start demo", 5))
    # Freeze screen
    pg.click("#dl-freeze + .sl")
    check("Freeze screen: the clock stops sending frames", wait(lambda: status()["displayFrozen"] is True, 5) and wait(lambda: "frozen" in pg.inner_text("#dl-blit"), 5), pg.inner_text("#dl-blit"))
    pg.click("#dl-freeze + .sl")
    check("... and goes on", wait(lambda: status()["displayFrozen"] is False, 5) and wait(lambda: "ms" in pg.inner_text("#dl-blit"), 5))
    # Hebcal data
    said = ui.click_msg("button[onclick='refreshCalendarData()']")
    check("Refresh now", said and "fail" not in said.lower(), said)
    ui.dialog_answer[0] = False
    n0 = len(ui.dialogs)
    pg.click("button[onclick='clearCalendarCache()']")
    pg.wait_for_timeout(500)
    check("Clear cache asks first (No: nothing)", any("Clear the calendar cache" in m for _, m in ui.dialogs[n0:]))
    ui.dialog_answer[0] = True
    said = ui.click_msg("button[onclick='clearCalendarCache()']")
    check("... Yes: cleared, the page reloads", "Success" in said, said)
    pg.wait_for_timeout(2500)
    ui.goto(HOME + "/")
    ui.settings()
    check("... the clock fetches it again", wait(lambda: status()["synced"] and screen() != "", 60, 1))
    # Log level: every level (until restart: not saved), then for how long
    saved = lambda: jget("/api/backup")["config"]
    for lvl in ("debug", "warn", "error", "none", "info"):
        pg.select_option("#cfg-log-level", lvl)
        check("log level %s, until restart (not saved)" % lvl, wait(lambda: status()["logLevel"] == lvl, 5)
              and (lvl == "info" or status()["logLevelKeep"] == "restart") and saved().get("logLevel") == "info", (status()["logLevel"], status().get("logLevelKeep")))
    pg.select_option("#cfg-log-level", "warn")
    for hours in ("1", "24", "168"):
        pg.select_option("#cfg-log-keep", hours)
        until = lambda: saved().get("logLevelUntil", 0)
        check("warn for %s h: saved, with its end" % hours, wait(lambda: saved().get("logLevel") == "warn" and abs(until() - time.time() - int(hours) * 3600) < 300, 6)
              and status()["logLevelKeep"] == "until", (saved().get("logLevel"), until() - time.time()))
        check("... the end shown beside it", wait(lambda: pg.inner_text("#log-keep-note").startswith("until "), 8), pg.inner_text("#log-keep-note"))
    # its time up: back to Info by itself - and a restart keeps it until then
    pg.select_option("#cfg-log-keep", "1")
    wait(lambda: abs(saved().get("logLevelUntil", 0) - time.time() - 3600) < 300, 6)
    clock.stop()
    cf = clock.fs / "config.json"
    c = json.loads(cf.read_text(encoding="utf-8"))
    c["logLevelUntil"] = int(time.time()) + 5
    cf.write_text(json.dumps(c, ensure_ascii=False), encoding="utf-8")
    clock.launch()
    check("after a restart: still warn (its time not up yet)", wait(up, 60, 1) and status()["logLevel"] == "warn", status().get("logLevel"))
    check("... its time up: back to Info by itself, saved so", wait(lambda: status()["logLevel"] == "info" and saved().get("logLevel") == "info"
          and saved().get("logLevelUntil") == 0, 150, 2), (status().get("logLevel"), saved().get("logLevel")))
    wait(synced, 90, 1)
    ui.goto(HOME + "/")
    ui.settings()
    # always: Debug asks first (the log fills sooner); other levels don't
    pg.select_option("#cfg-log-level", "debug")
    wait(lambda: status()["logLevel"] == "debug", 5)
    pg.select_option("#cfg-log-keep", "always")
    pg.wait_for_timeout(500)
    check("Debug always: asks first, saying why", modal_open(pg, "modal-confirm") and "older lines" in pg.inner_text("#cf-body"))
    ui.ask_answer("Cancel")
    pg.wait_for_timeout(1500)
    check("... Cancel: not saved", saved().get("logLevel") == "info" and pg.input_value("#cfg-log-keep") == "0", (saved().get("logLevel"), pg.input_value("#cfg-log-keep")))
    pg.select_option("#cfg-log-keep", "always")
    ui.ask_answer("Debug always")
    check("... Debug always: saved, no end", wait(lambda: saved().get("logLevel") == "debug" and saved().get("logLevelUntil") == 0 and status()["logLevelKeep"] == "always", 6))
    pg.select_option("#cfg-log-level", "info")
    check("Info always: back to normal, no question", wait(lambda: saved().get("logLevel") == "info", 6) and not modal_open(pg, "modal-confirm"))
    pg.select_option("#cfg-log-keep", "0")
    pg.wait_for_timeout(800)
    # The log: shown by itself when it comes into view; both files, the previous one first
    ui.goto(HOME + "/")
    ui.settings()
    pg.locator("#diag-log-view").scroll_into_view_if_needed()
    check("the log: shown by itself when it comes into view", wait(lambda: len(pg.inner_text("#diag-log-view")) > 200, 10))
    note = lambda m: http(HOME, "/api/debug/diagnostic-log/note", form={"msg": m})
    filler = "x" * 150   # a log line holds up to ~220 bytes
    cur_len = lambda: len(http(HOME, "/api/debug/diagnostic-log"))
    def to_new_file(limit=2000):
        """Notes until the clock starts a new log file (its current one small again)."""
        last, i = cur_len(), 0
        while i < limit:
            for _ in range(25):
                note("UIT filler %04d %s" % (i, filler)); i += 1
            n_ = cur_len()
            if n_ < last:
                return True
            last = n_
        return False
    to_new_file()
    note("UIT-OLD-MARK")
    check("the clock starts a new log file when one is full (%d KB)" % 128, to_new_file())
    note("UIT-NEW-MARK")
    cur = http(HOME, "/api/debug/diagnostic-log").decode("utf-8", "replace")
    old = http(HOME, "/api/debug/diagnostic-log?part=old").decode("utf-8", "replace")
    check("the clock: the mark now in its previous file, the newest in the current one", "UIT-OLD-MARK" in old and "UIT-OLD-MARK" not in cur and "UIT-NEW-MARK" in cur,
          (len(old), len(cur)))
    pg.click("button[onclick='loadDiagLog()']")
    check("... Reload shows both, the previous file first", wait(lambda: "UIT-NEW-MARK" in pg.inner_text("#diag-log-view"), 10)
          and pg.inner_text("#diag-log-view").find("UIT-OLD-MARK") < pg.inner_text("#diag-log-view").find("UIT-NEW-MARK") and "UIT-OLD-MARK" in pg.inner_text("#diag-log-view"))
    pg.fill("#diag-log-filter", "uit-")   # the two marks (the filler above pushed everything else out)
    lines = [l for l in pg.inner_text("#diag-log-view").split("\n") if l.strip()]
    check("... Filter keeps the matching lines", len(lines) == 2 and all("uit-" in l.lower() for l in lines), lines[:3])
    pg.fill("#diag-log-filter", "zzqqxx")
    check("... nothing matching says so", pg.inner_text("#diag-log-view") == "(no matching lines)")
    pg.fill("#diag-log-filter", "")
    # Factory reset: asks (Cancel here - the real one is in setup_and_restore)
    pg.click("button[onclick='factoryReset()']")
    check("Factory reset asks, offering a backup first", modal_open(pg, "modal-confirm") and pg.locator("#cf-buttons button", has_text="Export backup & Reset").count() == 1)
    ui.ask_answer("Cancel")
    check("... Cancel: nothing erased", cfg()["wifiSSID"] != "" and len(events()) > 0)


@section
def settings_search(ui, clock):
    """Settings search: matching rows stay (and work), group titles match, nothing matching says so."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    vis = lambda sel: pg.locator(sel).is_visible()
    pg.fill("#settings-search", "scroll speed")
    check("'scroll speed': that row stays, others go", vis("#cfg-scroll-s") and not vis("#cfg-name") and not vis("#cfg-ntp0"))
    press_range(pg, "#cfg-scroll-s", ["Home"] + ["ArrowRight"] * 2)
    check("... and works there", wait_cfg("scrollSpeed", 3))
    pg.fill("#settings-search", "wifi")
    check("'wifi' (no hyphen) finds the Wi-Fi group", vis("#cfg-name") and not vis("#cfg-scroll-s"))
    pg.fill("#settings-search", "omer")
    check("'omer' finds its Calendar Content row", pg.locator('#content-order-list .content-row[data-type="omer"]').is_visible()
          and not pg.locator('#content-order-list .content-row[data-type="parasha"]').is_visible())
    pg.fill("#settings-search", "brightness")
    check("'brightness' (a word in a tooltip) finds the sliders", vis("#dr-bright"))
    pg.fill("#settings-search", "zzqqxx")
    check("no match: says so", vis("#settings-search-none"))
    pg.fill("#settings-search", "")
    check("cleared: everything back", vis("#cfg-name") and vis("#cfg-scroll-s") and not vis("#settings-search-none"))

def make_package():
    """An update package of the current firmware build, in this run's folder (never the release folder).
    None if the simulator has no build of that version (it runs an update as the clock would: that version's code)."""
    r = subprocess.run([sys.executable, str(ROOT / "utils" / "make_update_package.py"), "--out", str(OUT / "files")],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode:
        log("package: %s" % (r.stdout + r.stderr).strip()[-300:])
    pk = sorted((OUT / "files").glob("JewishClock-update-*.jcupdate"))
    if not pk:
        return None, None
    v = pk[-1].name[len("JewishClock-update-"):-len(".jcupdate")]
    return pk[-1], v


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def in_setup_mode():
    try:
        r = urllib.request.build_opener(_NoRedirect).open(AP + "/wifi-scan", timeout=4)
        b = r.read()
        return r.status == 200 and b.lstrip().startswith(b"{") and b"networks" in b
    except Exception:
        return False


@section
def manuals(ui, clock):
    """Quick start guide and Manual: their contents links, Back to the clock's page."""
    pg = ui.page
    for page in ("/manual.html", "/start.html"):
        ui.goto(HOME + page)
        links = pg.locator("a[href^='#']")
        bad = []
        for i in range(links.count()):
            a = links.nth(i)
            target = a.get_attribute("href")
            a.click()
            pg.wait_for_timeout(150)
            if not pg.evaluate("location.hash") == target:
                bad.append(target)
        check("%s: every contents link goes to its part" % page, not bad, bad)
        if page == "/manual.html":
            pg.click("a[href='/start.html']")
            pg.wait_for_load_state()
            check("the manual's link to the Quick start guide", pg.url.endswith("/start.html"), pg.url)
        backs = pg.locator("a.back")
        for i in range(backs.count()):
            ui.goto(HOME + page)
            backs.nth(i).click()
            pg.wait_for_timeout(1500)
            check("%s: Back to the clock's page (%d)" % (page, i + 1), wait(lambda: pg.url.rstrip("/") == HOME and pg.is_visible("#view-calendar"), 5), pg.url)


@section
def update_page(ui, clock):
    """The update page: Back, choose a file, Update with a wrong file (refused) and a real package (installed, trial, Done); the clock's built-in safe update page too."""
    pg = ui.page
    pkg, pv = make_package()
    ui.goto(HOME + "/update.html")
    check("the version shown", pg.inner_text("#curVer") == status()["firmware"], pg.inner_text("#curVer"))
    check("Update is off until a file is chosen", pg.is_disabled("#goBtn"))
    with pg.expect_file_chooser() as fc:
        pg.click("#filePick")
    fc.value.set_files(f("notapic.txt"))
    check("a file chosen: its name, Update on", pg.inner_text("#filePick") == "notapic.txt" and not pg.is_disabled("#goBtn"))
    pg.click("#goBtn")
    check("a wrong file: refused, nothing changed", wait(lambda: "err" in (pg.get_attribute("#msg", "class") or ""), 20, 0.5), pg.inner_text("#msg"))
    pg.click("a.back")
    pg.wait_for_timeout(1000)
    check("Back: the clock's page", pg.url.rstrip("/") == HOME, pg.url)
    if not pkg or not (Path("C:/pio3/jc_clean/sim/versions") / pv / "jcclock.exe").exists():
        check("an update package of this build (pio run, then sim/build_clock.py: the same version)", False, (pkg, pv))
        return
    ui.goto(HOME + "/update.html")
    up0 = status()["uptimeMs"]
    with pg.expect_file_chooser() as fc:
        pg.click("#filePick")
    fc.value.set_files(str(pkg))
    pg.click("#goBtn")
    check("a real package: Done, the new version runs", wait(lambda: pg.inner_text("#msg").startswith("Done!"), 400, 2), pg.inner_text("#msg"))
    check("... the clock restarted into it, trial passed", status()["firmware"] == pv and status()["uptimeMs"] < up0 + 400000 and status()["updateState"] == "ok", status()["firmware"])
    wait(synced, 90, 1)
    # The built-in page (works even if the web files are broken)
    ui.goto(HOME + "/update-safe")
    pg.set_input_files("input[name=firmware]", f("notapic.txt"))
    pg.click("button[type=submit]")
    pg.wait_for_timeout(3000)
    check("safe update: a wrong file refused", "fail" in pg.content().lower() or "not" in pg.content().lower(), pg.inner_text("body")[:200])
    ui.goto(HOME + "/update-safe")
    up0 = status()["uptimeMs"]
    pg.set_input_files("input[name=firmware]", str(pkg))
    pg.click("button[type=submit]")
    check("safe update: a real package installed (restart, trial ok)", wait(lambda: status()["uptimeMs"] < up0 and status()["updateState"] == "ok" and status()["firmware"] == pv, 400, 2),
          status().get("updateState"))
    wait(synced, 90, 1)


@section
def setup_and_restore(ui, clock):
    """Reset Wi-Fi -> the setup page (network list, Show, Connect) -> back on the home Wi-Fi; Factory reset (with backup / without) -> Restore from backup."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    c0 = cfg()
    n_ev = len(events())
    home = {c0["wifiSSID"]: next(n["password"] for n in world()["networks"] if n["ssid"] == c0["wifiSSID"])}
    ui.dialog_answer[0] = False
    pg.click("button[onclick='confirmForget()']")
    pg.wait_for_timeout(1500)
    check("Reset Wi-Fi asks first (No: nothing)", cfg()["wifiSSID"] == c0["wifiSSID"] and not in_setup_mode())
    ui.dialog_answer[0] = True
    pg.click("button[onclick='confirmForget()']")
    check("Reset Wi-Fi: the clock opens its setup Wi-Fi", wait(in_setup_mode, 120, 2))
    # The setup page, on the clock's own Wi-Fi
    ui.goto(AP + "/", 2500)
    check("the setup page", "Setup" in pg.inner_text("h2"), pg.inner_text("body")[:100])
    check("... lists the networks around", wait(lambda: pg.locator("#ssidList option").count() >= 2, 30, 1), pg.locator("#ssidList option").count())
    for v in pg.eval_on_selector_all("#ssidList option", "os => os.map(o => o.value)"):
        pg.select_option("#ssidList", v)
        check("choosing %s fills the name" % v, pg.input_value("#ssidInput") == v)
    ssid = c0["wifiSSID"]
    pg.fill("#ssidInput", "")
    pg.fill("#ssidInput", ssid)   # typed (a network that doesn't show its name is typed in)
    pg.select_option("#ssidList", ssid)
    pg.fill("#passwordInput", home[ssid])
    pg.click("#togglePassword")
    check("Show: the password readable", pg.get_attribute("#passwordInput", "type") == "text")
    pg.click("#togglePassword")
    check("... Hide", pg.get_attribute("#passwordInput", "type") == "password")
    host = "%s.local" % c0["projectName"].lower()
    # .local can't be looked up on this PC: sent to the simulated clock
    def to_clock(route):   # as a phone back on the home Wi-Fi: answered once the clock is there
        wait(lambda: up() and synced(), 180, 1)
        route.fulfill(status=302, headers={"Location": HOME + "/"})
    pg.route("http://%s/**" % host, to_clock)
    pg.click("form#wifiForm button[type=submit]")
    pg.wait_for_timeout(2000)
    check("Connect: saved", "Saved" in pg.inner_text("body"), pg.inner_text("body")[:100])
    # Its link to the clock by name: .local can't be looked up on this PC - it is sent to the simulated clock
    link = pg.locator("a[href*='.local']")
    check("... a link to the clock by its name", link.count() == 1 and link.get_attribute("href") == "http://%s/" % host, link.get_attribute("href") if link.count() else None)
    if link.count():
        link.click(no_wait_after=True)
    check("... the clock joins the home Wi-Fi again", wait(lambda: sim_state().get("ssid") == ssid and up(), 120, 2) and wait(synced, 90, 1))
    try:
        pg.wait_for_url(HOME + "/", timeout=60000)   # lets the browser follow the link (a plain read of the address doesn't)
    except Exception:
        pass
    check("... the link (or the page by itself after 10 s) opens the clock's page", pg.url.rstrip("/") == HOME and wait(lambda: pg.is_visible("#view-calendar"), 10), pg.url)
    # Factory reset with a backup first
    ui.goto(HOME + "/")
    ui.settings()
    api_post("/api/config", {"scrollSpeed": 4})
    pg.wait_for_timeout(3000)
    c1 = cfg()
    pg.click("button[onclick='factoryReset()']")
    with pg.expect_download(timeout=60000) as d:
        ui.ask_answer("Export backup & Reset")
    backup = OUT / "files" / d.value.suggested_filename
    d.value.save_as(str(backup))
    check("Export backup & Reset: the backup file first", backup.exists() and backup.name.startswith("JewishClock-backup-"), backup.name)
    check("... then the clock is factory-new (setup Wi-Fi)", wait(in_setup_mode, 120, 2))
    # The setup page's Restore from backup
    ui.goto(AP + "/", 1500)
    pg.click("a[href='/restore']")
    pg.wait_for_timeout(1500)
    check("Restore from backup: its page", pg.is_visible("#restore-view") and not pg.is_visible("#view-calendar"))
    with pg.expect_file_chooser() as fc:
        pg.click("button:has-text('Choose backup file')")
    fc.value.set_files(f("notapic.txt"))
    pg.wait_for_timeout(800)
    check("not a backup: says so", "not a backup from this clock" in pg.inner_text("#restore-msg"), pg.inner_text("#restore-msg"))
    with pg.expect_file_chooser() as fc:
        pg.click("button:has-text('Choose backup file')")
    fc.value.set_files(str(backup))
    check("the backup: restored", wait(lambda: "Restored:" in pg.inner_text("#restore-msg"), 60, 1), pg.inner_text("#restore-msg"))
    check("... the clock restarts and joins its Wi-Fi", wait(lambda: sim_state().get("ssid") == c0["wifiSSID"] and up(), 120, 2) and wait(synced, 90, 1))
    c2 = cfg()
    diff = [k for k in c1 if k not in ("debugTimeOffset", "connectedSlot", "places") and c1[k] != c2.get(k)]
    check("... settings as before", not diff, [(k, c1[k], c2.get(k)) for k in diff])
    check("... events as before", len(events()) == n_ev, (len(events()), n_ev))
    # Factory reset without a backup
    ui.goto(HOME + "/")
    ui.settings()
    pg.click("button[onclick='factoryReset()']")
    ui.ask_answer("Reset without backup")
    check("Reset without backup: factory-new", wait(in_setup_mode, 120, 2))
    def no_events():   # in setup mode the clock serves only the setup page: look at its flash
        p = clock.fs / "events.json"
        return not p.exists() or json.loads(p.read_text(encoding="utf-8") or "[]") in ([], {"events": []})
    check("... nothing left (no events on its flash)", wait(no_events, 20, 1))

#SECTIONS_HERE


# ── main ──────────────────────────────────────────────────────────────────────
def report_coverage():
    seen = {k: v for k, v in SEEN.items() if k not in KNOWN_UNTESTABLE}
    unc = sorted(k for k in seen if k not in USED)
    log("COVERAGE: %d controls seen, %d used, %d uncovered, %d known untestable"
        % (len(seen), len(seen) - len(unc), len(unc), len(KNOWN_UNTESTABLE)))
    for k in unc:
        log("  UNCOVERED %-70s (%s %s)" % (k, seen[k]["where"], seen[k]["text"]))
    (OUT / "coverage.json").write_text(json.dumps({"seen": SEEN, "used": USED, "uncovered": unc}, ensure_ascii=False, indent=1), encoding="utf-8")
    return unc


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    names = [f.__name__ for f in SECTIONS]
    if "--list" in args:
        for f in SECTIONS:
            print("%-20s %s" % (f.__name__, (f.__doc__ or "").strip().split("\n")[0]))
        return
    only = args[args.index("--only") + 1].split(",") if "--only" in args else names
    if "--port-offset" in args:   # beside another run of the tests (run_all_tests.py --port-offset)
        global PORT_HOME, PORT_AP, PORT_CTL, HOME, AP, CTL
        off = int(args[args.index("--port-offset") + 1])
        PORT_HOME, PORT_AP, PORT_CTL = ports("ui", off)
        HOME, AP, CTL = ("http://127.0.0.1:%d" % p for p in (PORT_HOME, PORT_AP, PORT_CTL))
    OUT.mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright
    FILESDIR[0] = make_files()
    clock = Clock()
    try:
        if not wait(synced, 120, 1):
            sys.exit("the test clock did not come up synced - see %s" % (clock.dir / "run.log"))
        with sync_playwright() as pw:
            ui = UI(pw, show="--show" in args)
            ui._hook(ui.page)
            for f in SECTIONS:
                if f.__name__ not in only:
                    continue
                CURRENT[0] = f.__name__
                log("── %s: %s" % (f.__name__, (f.__doc__ or "").strip().split("\n")[0]))
                try:
                    f(ui, clock)
                except Exception as e:
                    check("section ran to the end", False, "%s: %s" % (type(e).__name__, e))
                    log(traceback.format_exc())
                harvest(ui.page)
            ui.close()
    finally:
        if "--keep" not in args:
            clock.stop()
    CURRENT[0] = "coverage"
    unc = report_coverage() if only == names else []
    failed = [r for r in results if not r[2]]
    log("RESULT: %d checks, %d failed%s" % (len(results), len(failed), ", %d uncovered" % len(unc) if only == names else ""))
    for r in failed:
        log("  FAILED [%s] %s %s" % (r[0], r[1], r[3][:200]))
    sys.exit(1 if failed or unc else 0)


if __name__ == "__main__":
    main()
