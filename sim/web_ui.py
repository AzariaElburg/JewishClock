"""
Every control of the clock's web pages, used in a browser the way a person
would, each result checked on the clock itself - not just "the click didn't
fail" (2026-11-07: the debug time's Hebrew Apply failed at the parents' and no
test had ever pressed it).

    C:/pio3/jc_clean/venv/Scripts/python sim/web_ui.py [--only SECTION[.PART],...] [--keep] [--show] [--port-offset N]
    (a part runs with the parts it needs - class Parts; --list shows them)
    C:/pio3/jc_clean/venv/Scripts/python sim/web_ui.py --list

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

Results: C:/pio3/jc_clean/sim/web_ui/<time>/summary.txt (+ a screenshot per failure).
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
OUT = Path("C:/pio3/jc_clean/sim/web_ui") / time.strftime("%Y%m%d_%H%M%S")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_ports import NO_FEED, no_real_feed, ports, urls  # noqa: E402
from time_relay import ensure_relay  # noqa: E402 - the test clocks' time server
from clock_facts import ClockWords, build_words, screen_text, test_random  # noqa: E402 - the clock's words: asked (Texts.h)
PORT_HOME, PORT_AP, PORT_CTL = ports("web_ui")   # sim_ports.py
HOME, AP, CTL = ("http://127.0.0.1:%d" % p for p in (PORT_HOME, PORT_AP, PORT_CTL))
FILES = ROOT / "sim" / "ui_files"   # test inputs (pictures, a video, Excel files)

results = []
PORT_OFF = [0]    # --port-offset (from_family's clock moves with it)
FROM = [(ROOT / "sim" / "family_version.txt").read_text(encoding="utf-8").strip()]   # --from: the family's version
PACKAGE = [None]  # --package: what from_family installs (default: made from the current build)


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


def clock_snapshot():
    """The clock's settings and events, to see what a section left changed (2026-11-16). Events by
    what they are, not their id (an import gives new ids); read once the clock answers (a section
    may end with a restart)."""
    wait(lambda: status().get("synced"), 60, 1)
    try:
        evs = {}
        for e in jget("/api/events/list"):
            evs.setdefault(ev_key(e), []).append(e.get("name"))
        return {"config": cfg(), "events": evs}
    except Exception as e:
        return {"error": str(e)}


# Settings the clock changes by itself (learned light, last network, places), not a section's leftovers
SNAPSHOT_SELF = {"wifiLastNetwork", "learnedLight", "textNotes", "roomLightPct"}   # roomLightPct: Learn room light moves it


def ev_key(e):
    return json.dumps({k: v for k, v in e.items() if k != "id"}, ensure_ascii=False, sort_keys=True)


def put_back(before):
    """Every section ends on the clock it started with (2026-11-16: calendar_content left every row
    at "every round", wifi the home network's place in another city - the next section in the
    lane ran on that, and which one that was depended on the lane balancing). Settings through
    /api/config, a network's place and the network it's on as the page sets them, the test events
    a section left deleted."""
    if "error" in before:
        return
    now = clock_snapshot()
    if "error" in now:
        return
    cb, cn = before["config"], now["config"]
    keys = {k: cb[k] for k in cb if k not in SNAPSHOT_SELF | {"places", "connectedSlot"} and cn.get(k) != cb[k]}
    if keys:
        api_post("/api/config", keys)
    for i, (pb, pn) in enumerate(zip(cb.get("places", []), cn.get("places", []))):
        if pb != pn and pb.get("set"):
            api_post("/api/location/place", dict({k: pb.get(k) for k in ("city", "country", "lat", "lon", "tzid", "offsetSec")}, slot=i))
    if cn.get("connectedSlot") != cb.get("connectedSlot") and (cb.get("connectedSlot") or -1) >= 0:
        api_post("/api/wifi/connect", {"network": cb["connectedSlot"] + 1})
        wait(lambda: status().get("synced") and cfg().get("connectedSlot") == cb["connectedSlot"], 60, 1)
    extra = {}
    for e in jget("/api/events/list"):
        k = ev_key(e)
        extra.setdefault(k, []).append(e)
    for k, evs in extra.items():
        for e in evs[len(before["events"].get(k, [])):]:
            if str(e.get("name", "")).startswith("UIT"):   # only the tests' own (their names start so)
                http(HOME, "/api/events/delete", form={"id": e["id"]}, raw=True)
    wait(lambda: not snapshot_diff(before, clock_snapshot()), 15, 0.5)


def snapshot_diff(a, b):
    if "error" in a or "error" in b:
        return ["(not read: %s)" % (a.get("error") or b.get("error"))]
    out = []
    ca, cb = a["config"], b["config"]
    for k in sorted(set(ca) | set(cb)):
        if k not in SNAPSHOT_SELF and ca.get(k) != cb.get(k):
            out.append("%s %s -> %s" % (k, json.dumps(ca.get(k), ensure_ascii=False)[:80], json.dumps(cb.get(k), ensure_ascii=False)[:80]))
    ea, eb = a["events"], b["events"]
    for k in sorted(set(ea) | set(eb)):
        na, nb = len(ea.get(k, [])), len(eb.get(k, []))
        if na != nb:
            out.append("event %s %s (%d -> %d)" % ("missing" if nb < na else "extra", (ea.get(k) or eb.get(k))[0], na, nb))
    return out


def calendar_settled(timeout=60):
    """The clock has today's downloaded calendar (not its own calculation meanwhile): today's
    Daf Yomi comes only from the download, every day (/api/calendar - the real clock's too).
    After a date change the clock fetches the new day's calendar; for about a second it then
    rightly knew no parasha, and a check planned in that second failed (2026-11-16)."""
    def ready():
        # ... for the clock's day: right after a date change (a section's debug time put back) the
        # calendar was still the old day's for a moment, Daf Yomi and all (2026-11-20)
        try:
            cal = jget("/api/calendar")
            return bool(cal.get("dafYomi")) and cal.get("gregorianDate") == status().get("date")
        except Exception:
            return False
    return wait(ready, timeout, 0.25)


def events():
    j = jget("/api/events/list")
    return j if isinstance(j, list) else j.get("events", [])


# The browser page tests run on (UI sets it). Playwright's sync API takes in the page's news
# (a navigation, pg.url) only while one of its own calls runs - a plain time.sleep() between
# reads of pg.url never sees the page move on (2026-11-14). So wait() pauses through it.
_PAGE = None


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
        try:
            if _PAGE is not None and not _PAGE.is_closed():
                _PAGE.wait_for_timeout(step * 1000)
                continue
        except Exception:
            pass
        time.sleep(step)


def drag_content_row(pg, key, onto):
    """Settings' slide order: a person's drag of row `key` onto row `onto` - hold the handle a
    moment, move at hand speed, let go once the rows have settled. 15 instant moves and an
    instant release (until 2026-11-09) dropped the row wherever the 150 ms animation happened
    to be - measured: 38 of 200 such drags landed as aimed, 300 of 300 at hand speed."""
    handle = pg.locator('#content-order-list .content-row[data-type="%s"] .drag-handle' % key)
    target = pg.locator('#content-order-list .content-row[data-type="%s"]' % onto)
    handle.scroll_into_view_if_needed()
    a, b = handle.bounding_box(), target.bounding_box()
    x, y0, y1 = a["x"] + a["width"] / 2, a["y"] + a["height"] / 2, b["y"] + b["height"] / 2
    pg.mouse.move(x, y0)
    pg.wait_for_timeout(150)
    pg.mouse.down()
    pg.wait_for_timeout(80)
    n = max(10, int(abs(y1 - y0) / 8))
    for i in range(1, n + 1):
        pg.mouse.move(x, y0 + (y1 - y0) * i / n)
        pg.wait_for_timeout(16)
    pg.wait_for_timeout(300)
    pg.mouse.up()
    pg.wait_for_timeout(1000)


def stays(cond, seconds, step=0.25):
    """A "must not change" check (the user, 2026-11-14): watches cond for the whole time and
    fails at the first moment it is not true (an error counts as not true) - instead of one
    look after a fixed wait, which misses a change that came and went. True = held throughout."""
    end = time.time() + seconds
    while True:
        try:
            if not cond():
                return False
        except Exception:
            return False
        if time.time() > end:
            return True
        try:
            if _PAGE is not None and not _PAGE.is_closed():
                _PAGE.wait_for_timeout(step * 1000)
                continue
        except Exception:
            pass
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


CLOCK = [None]   # this lane's test clock (main())
_WORDS = []


def W():
    """The words of the build under test (its --texts: Texts.h). A name not listed: a failed check."""
    if not _WORDS:
        _WORDS.append(build_words(EXE, missing=lambda n: check("the clock lists a text %r (--texts)" % n, False)))
    return _WORDS[0]


_PAGE_WORDS = []


def PW(pg=None):
    """The page's own messages (Data/index.html PAGE_TEXTS), read from the page under test - like W()
    for the clock's. pg: the page the check looks at (default: the lane's). A page without the list
    (an older version's) or a name not on it: a failed check."""
    if not _PAGE_WORDS:
        page = pg or PAGE[0]
        texts = page.evaluate("typeof PAGE_TEXTS === 'object' ? PAGE_TEXTS : null") if page else None
        if texts is None:
            check("the page lists its messages (PAGE_TEXTS)", False)
            return ClockWords({}, missing=lambda n: None)
        _PAGE_WORDS.append(ClockWords(texts, missing=lambda n: check("the page lists a text %r (PAGE_TEXTS)" % n, False)))
    return _PAGE_WORDS[0]


def clear_of_midnight(margin_s=120):
    """A section that takes "today" from the PC and checks it later doesn't start in the last
    `margin_s` before midnight: it waits until just after (2026-11-21: the night's run crossed 00:00
    mid-section - the day it checked was yesterday by then). Returns today's date."""
    now = datetime.datetime.now()
    left = (datetime.datetime.combine(now.date() + datetime.timedelta(days=1), datetime.time()) - now).total_seconds()
    if left < margin_s:
        log("   (%.0f s to midnight - waiting until just after it, the section checks today's date)" % left)
        time.sleep(left + 5)
    return datetime.date.today()


def starts():
    """How often this lane's clock has started: each start writes "[sim] JewishClock" to its run.log.
    A restart = one more - not "the uptime went down", which missed a restart when the uptime
    before was short (the step before had restarted it too - 2026-11-20, six places)."""
    try:
        return (CLOCK[0].dir / "run.log").read_text(encoding="utf-8", errors="replace").count("[sim] JewishClock ")
    except Exception:
        return 0


def restarted_since(n0):
    """It has started again since starts() was n0, and answers."""
    return starts() > n0 and up()


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
        no_real_feed(self.fs)
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
                                  env=dict(os.environ, JC_TITLE="Test (web-ui)"))

    def running_again(self):
        """After a section: its clock running and answering - a section that failed while it had
        the clock stopped (or mid-restart) took every later section in its lane down with it
        (2026-11-20). True: it was already."""
        was = self.p.poll() is None
        if not was:
            log("   (the section left its clock stopped - started again)")
            self.logf.close()
            self.launch()
        if not wait(lambda: up(), 120, 1):
            log("   (the clock doesn't answer 120 s after the section)")
        return was

    def stop(self):
        ctl("/stop")
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            pass
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.p.pid)], capture_output=True)
        # Really gone before anything starts on its ports: Windows ends it a moment after taskkill
        # returns (a drill started right after one found the port still taken - 2026-11-15)
        wait(lambda: not listening(PORT_CTL) and not listening(PORT_HOME) and not listening(PORT_AP), 15)
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
  const TEXTY = /^(text|number|search|password|date|time|datetime-local|range|email|url|tel|textarea|)$/;   // textarea: its type (2026-11-15)
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
SECTION_S = {}   # each section's seconds this run (--jobs uses the last run's to share them out)


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


# ── the page's code coverage (--js-coverage DIR, 2026-11-16) ─────────────────
# Which parts of the page's scripts ran, and how often, per section: the browser's own counter
# (Chrome DevTools Protocol, Profiler precise coverage, block level), taken - and so reset - at
# each section's end. DIR/<section>.json + DIR/src/<sha1>.js (the scripts, to turn offsets into
# lines). Read by sim/coverage_report.py.
# A page that is left (a link, a reload, goto) takes its counts along - Chrome reports only the
# scripts still loaded (tried 2026-11-17: page A, then B: A's counts gone). So every page is
# stopped at its beforeunload (an empty listener added for that - in a coverage run only) and its
# counts kept first; held at the network instead, Chrome never answered the counts request.
JSCOV = [None]


class JsCoverage:
    def __init__(self, out):
        self.out = Path(out)
        (self.out / "src").mkdir(parents=True, exist_ok=True)
        self.sessions = []   # (cdp session, {scriptId: url})
        self.src = {}        # (session, scriptId) -> sha1 of its source
        self.rows = []       # counts kept so far in this section (pages left among them)

    def watch(self, ctx, page):
        try:
            s = ctx.new_cdp_session(page)
            urls = {}
            s.on("Debugger.scriptParsed", lambda ev: urls.__setitem__(ev["scriptId"], ev.get("url", "")))

            def leaving(ev):   # stopped at beforeunload: this page's counts, then on
                try:
                    if "beforeunload" in json.dumps(ev.get("data", {})):
                        self.keep(s, urls)
                finally:
                    try:
                        s.send("Debugger.resume")
                    except Exception:
                        pass   # already going on (the page closed, or its tab was shut)
            s.on("Debugger.paused", leaving)
            s.send("Debugger.enable")
            s.send("DOMDebugger.setEventListenerBreakpoint", {"eventName": "beforeunload", "targetName": "*"})
            s.send("Profiler.enable")
            s.send("Profiler.startPreciseCoverage", {"callCount": True, "detailed": True})
            self.sessions.append((s, urls))
        except Exception as e:
            log("   (js coverage: page not watched: %s)" % e)

    def keep(self, s, urls):
        """This page's counts since the last take into self.rows; False if the page is gone."""
        import hashlib
        try:
            res = s.send("Profiler.takePreciseCoverage")["result"]
        except Exception:
            return False
        for sc in res:
            url = sc.get("url") or urls.get(sc["scriptId"], "")
            if not url.startswith("http://127.0.0.1"):
                continue   # the tests' own helpers, the browser's
            sha = self.src.get((id(s), sc["scriptId"]))
            if sha is None:
                try:
                    text = s.send("Debugger.getScriptSource", {"scriptId": sc["scriptId"]})["scriptSource"]
                except Exception:
                    continue
                sha = hashlib.sha1(text.encode("utf-8")).hexdigest()
                f = self.out / "src" / (sha + ".js")
                if not f.exists():
                    f.write_text(text, encoding="utf-8")
                self.src[(id(s), sc["scriptId"])] = sha
            self.rows.append({"url": url.split("?")[0], "sha": sha,
                              "functions": [{"name": fn["functionName"], "ranges": fn["ranges"]} for fn in sc["functions"]]})
        return True

    def take(self, section):
        for s, page_urls in list(self.sessions):
            if not self.keep(s, page_urls):
                self.sessions.remove((s, page_urls))   # the page is gone
        rows, self.rows = self.rows, []
        f = self.out / (section + ".json")
        old = json.loads(f.read_text(encoding="utf-8")) if f.exists() else []
        f.write_text(json.dumps(old + rows), encoding="utf-8")


# ── the browser ───────────────────────────────────────────────────────────────
class UI:
    def __init__(self, pw, show=False):
        self.browser = pw.chromium.launch(channel="msedge", headless=not show)
        self.ctx = self.browser.new_context(viewport={"width": 1280, "height": 900}, accept_downloads=True)
        self.ctx.expose_function("__covPush", self._cov)
        self.ctx.add_init_script(COVERAGE_JS)
        if JSCOV[0]:   # something to stop at when a page is left (JsCoverage)
            self.ctx.add_init_script("addEventListener('beforeunload', function () {})")
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
        global _PAGE
        _PAGE = self.page
        PAGE[0] = self.page

    def _cov(self, kind, key, info):
        if kind == "seen":
            SEEN.setdefault(key, info)
        else:
            USED[key] = USED.get(key, 0) + 1

    def _hook(self, page):
        if JSCOV[0]:
            JSCOV[0].watch(self.ctx, page)
        page.on("dialog", self._dialog)
        page.on("pageerror", lambda e: check("no script error on the page", False, str(e)[:300]))
        page.on("close", lambda p: None)

    def _dialog(self, d):
        self.dialogs.append((d.type, d.message))
        (d.accept if self.dialog_answer[0] else d.dismiss)()

    def goto(self, url, wait_ms=1500):
        harvest(self.page)
        try:
            self.page.goto(url, wait_until="load")
        except Exception as e:   # the page moving on by itself meanwhile (after a restore it goes to the clock's own Wi-Fi address;
            # after a restart it reloads itself - Chrome then says ERR_ABORTED, 2026-11-16); a real failure fails again below
            if "interrupted by another navigation" not in str(e) and "net::ERR_ABORTED" not in str(e):
                raise
            self.page.wait_for_timeout(1000)
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
# Sections that leave the clock factory-new (off the home Wi-Fi - put_back() can't reach it): last in
# their lane, whatever order they're defined in (2026-11-18: refusals after setup_and_restore found
# no clock).
LAST_IN_LANE = {"setup_and_restore"}


def section(fn):
    SECTIONS.append(fn)
    return fn


ONLY_PARTS = {}   # --only section.part: section -> the parts asked for (with what they need)


class Parts:
    """A section's named parts, each runnable alone: --only chains.24_hour runs that part
    and the parts it needs, nothing else (2026-11-11).

    Parts sharing a moment form a STAGE: its setup runs once (e.g. the clock at a
    Friday morning), its parts do their page steps and say what the clock's screen
    must show - expect() - or must not - expect_absent(); at the stage's end ONE watch
    of the screen checks them all (each slide passes once per round), instead of a
    wait per check."""

    def __init__(self, section):
        self.section, self.parts, self.stages, self.pending, self.stage_now = section, [], {}, [], None

    def stage(self, name, setup=None):
        """A moment the following parts share; setup() brings the clock there (once)."""
        self.stages[name] = setup
        return name

    def part(self, name, stage, needs=()):
        def reg(fn):
            self.parts.append((name, stage, tuple(needs), fn))
            return fn
        return reg

    def expect(self, label, pred, timeout=240):
        self.pending.append((label, pred, False, timeout))

    def expect_absent(self, label, pred):
        self.pending.append((label, pred, True, 0))

    ROUNDS_AHEAD = 4   # "must not show": absent from this many coming rounds ("every Nth round" rows included)

    def settle(self):
        """Everything the stage's parts expect, from the running clock's next rounds (2026-11-15):
        /state?rounds=N - its own round-building code with its live settings, events and day
        (MessageAggregator::upcomingRounds), instead of watching the slides go by in real time
        (37 of a run's 48 minutes). "Shows" = on a slide of the coming rounds - asked again until
        it is, up to the part's own time (a moment yet to come, e.g. midnight, still waits for it);
        "must not show" = on none of ROUNDS_AHEAD rounds, and still so 2 s later. That the coming
        rounds ARE what the screen shows is checked once a run (preview_matches_screen())."""
        if not self.pending:
            return
        want = [p for p in self.pending if not p[2]]
        absent = [p for p in self.pending if p[2]]
        n = self.ROUNDS_AHEAD

        def slides():
            return [s for r in clock_state_rounds(n) for s in r]
        found, last = {}, []

        def all_found():
            last[:] = slides()
            for label, pred, _, _ in want:
                if label not in found:
                    hit = next((s for s in last if s and pred(s)), None)
                    if hit:
                        found[label] = hit
            return len(found) == len(want)
        wait(all_found, max([p[3] for p in want] + [20]), 0.3)

        def none_bad():
            return not any(pred(s) for _, pred, _, _ in absent for s in slides() if s)
        gone = not absent or (wait(none_bad, 20, 0.3) and stays(none_bad, 2, 0.5))
        bad = {label: next((s for s in slides() if s and pred(s)), None) for label, pred, _, _ in absent}
        for label, _, is_absent, _ in self.pending:
            if is_absent:
                check(label, gone and not bad[label], bad[label] or "on none of the next %d rounds" % n)
            else:
                check(label, label in found, found.get(label) or last[-6:])
        self.pending = []

    def needed(self, names):
        """The asked parts and everything they need, in the order they were declared."""
        want, todo = set(), list(names)
        by = {p[0]: p for p in self.parts}
        while todo:
            n = todo.pop()
            if n not in by:
                raise SystemExit("no part %s.%s (--list)" % (self.section, n))
            if n not in want:
                want.add(n)
                todo += by[n][2]
        return [p for p in self.parts if p[0] in want]

    def run(self):
        asked = ONLY_PARTS.get(self.section)
        for name, stage, _, fn in self.needed(asked) if asked else self.parts:
            if stage != self.stage_now:
                self.settle()
                self.stage_now = stage
                if self.stages.get(stage):
                    log("   ~ %s" % stage)
                    self.stages[stage]()
            log("   · %s.%s" % (self.section, name))
            try:
                fn()
            except Exception as e:
                check("%s: part ran to the end" % name, False, "%s: %s" % (type(e).__name__, e))
                log(traceback.format_exc())
        self.settle()


def clock_state():
    try:
        return json.loads(ctl("/state") or b"{}")
    except ValueError:
        return {}


def screen():
    return clock_state().get("screen", "")


def clock_state_rounds(n):
    """The clock's next n rounds, each a list of slides in the "screen" form (sim /state?rounds=n)."""
    try:
        return json.loads(ctl("/state?rounds=%d" % n) or b"{}").get("nextRounds", [])
    except ValueError:
        return []


def preview_matches_screen(timeout=300):
    """Once a run, in real time: the clock's next round as /state?rounds=1 tells it is what its
    screen then shows, slide by slide (what lets Parts.settle() ask instead of watching).
    Returns (True/False, detail)."""
    # Asked again and again until the round starts: the plan is what the clock knows NOW - asked
    # once right after a start, the calendar's download came in between and the round rightly
    # had one slide more (2026-11-15). The last plan before the round = what it was built from.
    r0 = clock_state().get("rounds", 0)
    plan = []

    def started():
        st = json.loads(ctl("/state?rounds=1") or b"{}")
        if st.get("rounds", 0) > r0:
            return True
        plan[:] = (st.get("nextRounds") or [[]])[0]
        return False
    if not wait(started, timeout, 0.1):
        return False, "no new round in %d s" % timeout
    shown, end = [], time.time() + timeout
    while time.time() < end:
        t = screen()
        if clock_state().get("rounds", 0) != r0 + 1:   # read, then still this round: not the next one's first slide
            break
        if t and (not shown or shown[-1] != t):
            shown.append(t)
        time.sleep(0.2)
    return shown == plan, {"planned": plan, "shown": shown}


def light_settled(at_least=0):
    """The backlight's output (0..1, the simulator's PWM) once the clock's fade to a new
    level is over: unchanged for 2 s (after at_least seconds - room light is averaged)."""
    time.sleep(at_least)
    vals = []
    end = time.time() + 40
    while time.time() < end:
        vals.append(clock_state().get("backlight", -1))
        if len(vals) >= 5 and len(set(vals[-5:])) == 1:
            break
        time.sleep(0.5)
    return vals[-1]


def never_rounds(bad, n, timeout=240):
    """"Must not show" on the clock's screen: on none of its next n rounds (asked - /state?rounds,
    2026-11-15; watched in real time before), once the change has reached the clock (up to 20 s),
    and still so 2 s later. timeout: kept for the callers, unused."""
    def none_bad():
        return not any(s and bad(s) for r in clock_state_rounds(max(n, 2)) for s in r)
    return bool(wait(none_bad, 20, 0.3) and stays(none_bad, 2, 0.5))


def screen_shows(pred, timeout=60):
    """Until a slide pred() accepts is on the screen now or in its next rounds (asked -
    /state?rounds, 2026-11-15; watched in real time before); returns it (or None) - and the
    slides it last had coming."""
    seen = []

    def hit():
        seen[:] = [screen()] + [s for r in clock_state_rounds(Parts.ROUNDS_AHEAD) for s in r]
        return next((s for s in seen if s and pred(s)), None)
    t = wait(hit, timeout, 0.3)
    return t, seen


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
    """A slider moved by keyboard (real input events, as a person using arrows / Home / End).
    Focused again before every key: when the page redraws the slider in between (a live save's
    answer arriving), the rest of the keys went nowhere - seen under a full run's load (2026-11-15)."""
    for k in keys:
        pg.focus(selector)
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
    today = clear_of_midnight()
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
    check("its chip opens it for editing", wait(lambda: modal_open(pg) and pg.input_value("#ev-name") == "UIT Chip" and pg.inner_text("#modal-title") == "Edit Event", 10))
    pg.click("#modal-event .close-x")
    check("✕ closes the dialog", not modal_open(pg))
    # Hebrew view
    pg.click("#btn-view-h")
    check("Heb: days in letters", wait(lambda: pg.locator(".daylbl").first.inner_text() == "א׳", 10), pg.locator(".daylbl").first.inner_text())
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
    check("back to Gregorian: days in numbers", wait(lambda: pg.locator(".daylbl").first.inner_text() == "1", 10))
    ui.settings()
    check("the Settings tab", pg.is_visible("#view-settings") and not pg.is_visible("#view-calendar"))
    ui.calendar()
    check("the Calendar tab", pg.is_visible("#view-calendar") and not pg.is_visible("#view-settings"))
    pg.click("text=Current Month")
    clock_day = int(jget("/api/status")["date"][8:10])   # the clock's today, now - not the PC's at the section's start
    check("today's row marked (Gregorian)", pg.locator(".dayrow.today .daylbl").inner_text() == str(clock_day))   # (a run across midnight, 2026-11-21)
    # A day's row: a new event on that day
    pg.locator(".dayrow").nth(4).click()
    check("a day's row: new event on that day", wait(lambda: modal_open(pg) and pg.input_value("#ev-g-d") == "5" and pg.inner_text("#modal-title") == "New Event", 10))
    pg.locator("#modal-event button", has_text="Cancel").click()
    check("Cancel closes the dialog", not modal_open(pg))
    # Search
    pg.fill("#search-input", "UIT Ch")
    items = pg.locator("#search-results .search-item")
    check("search finds by name", wait(lambda: items.count() == 1, 10, 0.2) and "UIT Chip" in items.first.inner_text(), items.all_inner_texts())
    items.first.click()
    check("a result opens that event", wait(lambda: modal_open(pg) and pg.input_value("#ev-name") == "UIT Chip" and pg.input_value("#search-input") == "", 10))
    pg.keyboard.press("Escape")
    check("Escape closes the dialog", not modal_open(pg))
    pg.fill("#search-input", "UIT")
    check("search: several results", wait(lambda: pg.locator("#search-results .search-item").count() == 2, 10))
    pg.fill("#search-input", "zzqqxx")
    check("search: no match says so", wait(lambda: pg.inner_text("#search-results") == "No matches", 10))
    pg.locator("#search-results .search-item").click()
    pg.fill("#search-input", "")
    check("search cleared: list gone", wait(lambda: not pg.is_visible("#search-results"), 10))
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
    check("... and stays (watched for 6 s, never gone)", stays(lambda: "show" in (pg.get_attribute("#toast", "class") or ""), 6))
    pg.evaluate("el('toast').classList.add('test-hit')")
    pg.click("#toast")
    check("a click on it closes it", wait(lambda: "show" not in (pg.get_attribute("#toast", "class") or ""), 10))
    pg.evaluate("el('toast').classList.remove('test-hit'); toast('another')")
    pg.wait_for_timeout(600)
    pg.mouse.click(640, 12)   # the header bar: nothing there to open
    check("... so does a click anywhere else", wait(lambda: "show" not in (pg.get_attribute("#toast", "class") or ""), 10))
    # A click beside the dialog closes it
    pg.click("button.fab")
    pg.wait_for_timeout(500)
    pg.mouse.click(5, 450)
    check("a click beside the dialog closes it", wait(lambda: not modal_open(pg), 10))
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
    # The page asks the clock for each conversion: waited for until shown (fixed 1 s waits before 2026-11-15)
    h = convert(**{"from": "g", "y": 1990, "m": 3, "d": 14})
    check("Gregorian typed: Hebrew filled in", wait(lambda: pg.input_value("#ev-j-m") == h["month"] and pg.input_value("#ev-j-d") == heb_num(h["d"])
          and pg.input_value("#ev-j-y") == heb_num(h["y"] - 5000), 15, 0.2), (pg.input_value("#ev-j-d"), pg.input_value("#ev-j-m"), pg.input_value("#ev-j-y"), h))
    check("typing a date ticks its calendar", pg.is_checked("#ev-has-g"))
    pg.check("#ev-as")
    h2 = convert(**{"from": "g", "y": 1990, "m": 3, "d": 14, "afterSunset": True})
    check("After sunset: the next Hebrew day", wait(lambda: pg.input_value("#ev-j-d") == heb_num(h2["d"]), 15, 0.2), (pg.input_value("#ev-j-d"), h2))
    pg.uncheck("#ev-as")
    check("After sunset off: back", wait(lambda: pg.input_value("#ev-j-d") == heb_num(h["d"]), 15, 0.2))
    # Hebrew side: every month; Adar shows Adar II
    for m in [""] + H_MONTHS:
        pg.select_option("#ev-j-m", m)
        if m == "אדר":
            check("Adar: the Adar II box appears", pg.is_visible("#row-adar2"))
            pg.check("#ev-is-adar2")
            pg.uncheck("#ev-is-adar2")
    check("other months: no Adar II box", not pg.is_visible("#row-adar2"))
    # Adar II converts as Adar II, and ticking it converts again (2026-11-16: the tick was never
    # sent - 14 Adar II 5784 got 14 Adar I's date): Purim 5784 = 24 Mar 2024, Purim Katan 23 Feb
    gval = lambda: (pg.input_value("#ev-g-d"), pg.input_value("#ev-g-m"), pg.input_value("#ev-g-y"))
    pg.fill("#ev-j-d", "14"); pg.select_option("#ev-j-m", "אדר"); pg.fill("#ev-j-y", "5784"); pg.click("#ev-name")
    check("14 Adar (I) 5784: 23 Feb 2024", wait(lambda: gval() == ("23", "2", "2024"), 15, 0.2), gval())
    pg.check("#ev-is-adar2")
    check("... Adar II ticked: 24 Mar 2024 (Purim)", wait(lambda: gval() == ("24", "3", "2024"), 15, 0.2), gval())
    pg.uncheck("#ev-is-adar2")
    check("... untick: back to 23 Feb", wait(lambda: gval() == ("23", "2", "2024"), 15, 0.2), gval())
    # Hebrew typed in letters, normalized on leaving the field; Gregorian follows
    pg.uncheck("#ev-has-g")
    pg.fill("#ev-j-d", "כג")
    pg.select_option("#ev-j-m", "ניסן")
    pg.fill("#ev-j-y", "תשנ")
    pg.click("#ev-name")
    g = convert(**{"from": "h", "y": 5750, "month": "ניסן", "d": 23})
    gval = lambda: (pg.input_value("#ev-g-d"), pg.input_value("#ev-g-m"), pg.input_value("#ev-g-y"))
    # The page asks the clock - under a full run's load that took over the 1 s once waited here (2026-11-15)
    filled = wait(lambda: gval() == (str(g["d"]), str(g["m"]), str(g["y"])), 15, 0.2)
    check("Hebrew typed in letters, shown as letters", pg.input_value("#ev-j-d") == "כ״ג" and pg.input_value("#ev-j-y") == heb_num(750))
    check("Hebrew typed: Gregorian filled in", filled, (g, gval()))
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
    g16 = convert(**{"from": "h", "y": 5750, "month": "ניסן", "d": 16})
    check("Hebrew day ▴: 15 -> ט״ז, the Gregorian date follows", wait(lambda: pg.input_value("#ev-j-d") == "ט״ז" and pg.input_value("#ev-g-d") == str(g16["d"]), 15, 0.2),
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
        check("... and Save refuses it", PW().has("notSaved", said) and words in said and modal_open(pg) and not find_event("UIT Spin"), said)
        pg.fill("#" + field, "טו" if field == "ev-j-d" else "תשנ")
        pg.click("#ev-name")
    # A Gregorian year in the Hebrew year box: "Did you mean ...?" (that date in 2027 is in 5787)
    gd, gm = int(pg.input_value("#ev-g-d")), int(pg.input_value("#ev-g-m"))
    want = convert(**{"from": "g", "y": 2027, "m": gm, "d": gd})["y"]
    pg.fill("#ev-j-y", "2027")
    pg.click("#ev-name")
    check("2027 in the Hebrew year box: asks 'Did you mean %s?'" % heb_num(want - 5000),
          wait(lambda: modal_open(pg, "modal-confirm") and ("Did you mean " + heb_num(want - 5000)) in pg.inner_text("#cf-title"), 10), pg.inner_text("#cf-title"))
    ui.ask_answer("Cancel")
    check("... Cancel: left as typed, red", wait(lambda: pg.input_value("#ev-j-y") == "2027" and "field-missing" in (pg.get_attribute("#ev-j-y", "class") or ""), 10))
    pg.click("#modal-event button:has-text('Save')")
    check("... Save asks again", wait(lambda: modal_open(pg, "modal-confirm") and "Did you mean" in pg.inner_text("#cf-title"), 10))
    ui.clear_toast()
    ui.ask_answer("Cancel")
    said = ui.wait_toast()
    check("... Cancel: not saved, says why", PW().has("notSaved", said) and PW().has("looksGregorian", said) and not find_event("UIT Spin"), said)
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
    # the Hebrew side filled in by the conversion first (a fixed 1 s, until 2026-11-16: under load
    # the answer came after the slip was typed and overwrote it - the page now drops such answers)
    wait(lambda: pg.input_value("#ev-j-m") == "אדר" and pg.input_value("#ev-j-d") == heb_num(17), 15, 0.2)
    pg.fill("#ev-j-y", ""); pg.fill("#ev-j-d", "20"); pg.click("#ev-name")
    pg.wait_for_timeout(500)
    said = ui.click_msg(save)
    check("typed: Gregorian with year, Hebrew day wrong (no Hebrew year): refused", PW().has("dateMismatch", said) and "י״ז אדר" in said and modal_open(pg), said)
    pg.fill("#ev-j-d", "17"); pg.click("#ev-name")
    pg.click(save)
    e = wait(lambda: find_event("UIT OnlyG"), 15)   # was a fixed 1500 ms (2026-11-14)
    check("... corrected: saved", e and e["jD"] == 17 and e["jYNum"] == 0 and e["gY"] == 1990, e)
    # only the Hebrew year: 17 Adar 5750 = 14 Mar; the Gregorian year cleared, day typed 20
    new_event("UIT OnlyH")
    pg.fill("#ev-j-d", "17"); pg.select_option("#ev-j-m", "אדר"); pg.fill("#ev-j-y", "5750"); pg.click("#ev-name")
    wait(lambda: pg.input_value("#ev-g-y") == "1990" and pg.input_value("#ev-g-d") == "14", 15, 0.2)   # converted first
    pg.fill("#ev-g-y", ""); pg.fill("#ev-g-d", "20")
    pg.wait_for_timeout(500)
    said = ui.click_msg(save)
    check("typed: Hebrew with year, Gregorian day wrong (no Gregorian year): refused", PW().has("dateMismatch", said) and "14/3" in said and modal_open(pg), said)
    # ... the evening before (13 Mar): offers After sunset
    pg.fill("#ev-g-d", "13")
    pg.wait_for_timeout(300)
    ui.dialog_answer[0] = False
    n0 = len(ui.dialogs)
    said = ui.click_msg(save)
    check("... Gregorian = the evening before: offers After sunset (No = not saved)", any("After Sunset" in m for _, m in ui.dialogs[n0:]) and PW().has("dateMismatch", said) and modal_open(pg),
          (ui.dialogs[n0:], said))
    ui.dialog_answer[0] = True
    pg.click(save)
    e = wait(lambda: find_event("UIT OnlyH"), 15)   # was a fixed 1500 ms (2026-11-14)
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
          W().has("dateNeverMeet", said) and "ה׳ ניסן" in said and modal_open(pg) and not find_event("UIT NoYearsBad"), said)
    pg.locator("#modal-event button", has_text="Cancel").click()
    no_years("UIT NoYears", 14, 3, 20, "אדר")
    pg.click(save)
    e = wait(lambda: find_event("UIT NoYears"), 15)   # was a fixed 1500 ms (2026-11-14)
    check("typed: no years, 14 March + 20 Adar (meet in some years): saved", e and (e["gD"], e["gM"], e["jD"], e["jMonth"]) == (14, 3, 20, "אדר"), e)
    # Stored that way (an Excel import saves and warns): a wrong day, a wrong year
    r = store_events(dict(name="UIT Mismatch", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h["d"] + 3, jMonth=h["month"], jYNum=5750, jY="תש״נ"),
                     dict(name="UIT WrongYear", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h["d"], jMonth=h["month"], jYNum=5751, jY="תשנ״א"))
    check("an import stores mismatched dates, with a warning each", r.get("saved") and len(r.get("warnings", [])) == 2, r)
    add_event(name="UIT Sunset", hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=h2["d"], jMonth=h2["month"], jYNum=5750, jY="תש״נ")
    for name, what in [("UIT Mismatch", "a wrong day"), ("UIT WrongYear", "a wrong year")]:
        open_event(pg, name)
        said = ui.click_msg(save)
        check("stored with %s: Save refused" % what, PW().has("dateMismatch", said) and modal_open(pg), said)
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
    check("Delete asks first", wait(lambda: modal_open(pg, "modal-confirm") and "UIT Full Renamed" in pg.inner_text("#cf-title"), 10), pg.inner_text("#cf-title"))
    ui.ask_answer("Cancel")
    check("... Cancel: kept", find_event("UIT Full Renamed") is not None and modal_open(pg))
    pg.click("#btn-del")
    pg.wait_for_timeout(300)
    ui.ask_answer("Delete")
    check("... Delete: gone, in the recycle bin", wait(lambda: find_event("UIT Full Renamed") is None
          and any(x["name"] == "UIT Full Renamed" for x in jget("/api/events/deleted")), 15, 0.2))
    pg.locator("#toast button", has_text="Undo").click()
    check("Undo brings it back", wait(lambda: find_event("UIT Full Renamed") is not None and not any(x["name"] == "UIT Full Renamed" for x in jget("/api/events/deleted")), 15))   # was a fixed 1000 ms (2026-11-14)
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
    trim = "#ev-photo-trim"
    check("a video: its player shows, the message is gone", wait(lambda: pg.is_visible(trim + " video") and not pg.is_visible("#ev-photo-msg"), 15, 0.2))
    t = lambda: pg.evaluate("document.querySelector('#ev-photo-trim video').currentTime")
    for a_, want in [("+1", 1.0), ("+0.1", 1.1), ("-0.1", 1.0), ("-1", 0.0)]:
        pg.click(trim + ' button[data-a="%s"]' % a_)
        check("video %s s" % a_, wait(lambda: abs(t() - want) < 0.05, 10), t())
    pg.fill(trim + " .trim-time", "0:02.5")
    pg.press(trim + " .trim-time", "Enter")
    check("a typed moment (0:02.5 + Enter)", wait(lambda: abs(t() - 2.5) < 0.05, 10), t())
    press_range(pg, trim + " .trim-bar input", ["ArrowLeft"] * 50)
    check("the moment slider", wait(lambda: t() < 2.4, 10), t())
    pg.click(trim + ' button[data-a="play"]')
    pg.wait_for_timeout(700)
    playing = not pg.evaluate("document.querySelector('#ev-photo-trim video').paused")
    pg.click(trim + ' button[data-a="play"]')
    check("▶ plays, ❚❚ stops", playing and pg.evaluate("document.querySelector('#ev-photo-trim video').paused"))
    check("the picture is the moment shown", wait(lambda: pg.evaluate("!!photo.img && photo.changed"), 10))
    pg.click("#modal-event button:has-text('Save')")
    e = wait(lambda: find_event("UIT Pic"), 15)   # was a fixed 2000 ms (2026-11-14)
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
    items = pg.locator("#ev-gallery .gal-item")
    check("Library lists the clock's pictures", wait(lambda: items.count() == len(lib1), 15, 0.2), (items.count(), len(lib1)))
    pg.click("text=Library")
    check("Library again closes it", not pg.is_visible("#ev-gallery"))
    pg.click("text=Library")
    k = [i for i, x in enumerate(jget("/api/images/list")) if x["id"] == e["image"]][0]
    wait(lambda: items.count() > k, 15, 0.2)
    items.nth(k).click()
    check("a library picture chosen", wait(lambda: pg.evaluate("photo.imageId") == e["image"] and not pg.is_visible("#ev-gallery"), 10, 0.2))
    pg.click("#modal-event button:has-text('Save')")
    e2 = wait(lambda: find_event("UIT Pic2"), 15)   # was a fixed 1500 ms (2026-11-14)
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
    check("a name opens that event", wait(lambda: modal_open(pg) and pg.input_value("#ev-name") == "UIT Pic2", 10), pg.input_value("#ev-name"))
    pg.locator("#modal-event button", has_text="Cancel").click()
    check("... Cancel: back on Settings", wait(lambda: not modal_open(pg) and on_settings(), 10))
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
    got = {"lines": []}
    def deleted_listed():
        got["lines"] = pic_tile_here()[2]
        return any(l.endswith("UIT Pic2 Renamed (deleted)") for l in got["lines"])
    check("... Delete: back on Settings, the event listed as (deleted)", wait(deleted_listed, 15, 0.3) and on_settings() and not modal_open(pg),
          got["lines"])
    http(HOME, "/api/events/restore", form={"id": e2["id"]})
    ui.calendar()
    # Opening an event with a picture shows it
    open_event(pg, "UIT Pic")
    check("an event's picture shows when it's opened", pg.is_visible("#ev-photo-canvas") and pg.evaluate("photo.imageId") == e["image"])
    pg.click("#ev-photo-remove")
    pg.click("#modal-event button:has-text('Save')")
    check("picture removed and saved: the event has none", wait(lambda: find_event("UIT Pic")["image"] == "", 15))   # was a fixed 1500 ms (2026-11-14)
    remove_test_events()
    check("pictures no event uses leave the library", wait(lambda: e["image"] not in {x["id"] for x in jget("/api/images/list")}, 15, 0.3))


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
    rows = pg.locator("#bin-list .frow")
    wait(lambda: rows.count() == 4, 10, 0.2)
    check("Deleted events lists them, newest first", rows.count() == 4 and "UIT Bin3" in rows.first.inner_text(), rows.all_inner_texts())
    check("... with their dates", "4.3" in rows.first.inner_text())
    check("Restore selected: off until something is ticked", pg.is_disabled("#bin-restore-sel"))
    pg.locator('#bin-list [data-restore="%s"]' % ids[0]).click()
    check("Restore (one)", wait(lambda: find_event("UIT Bin0") is not None and rows.count() == 3, 10, 0.2), rows.count())
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[1])
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.uncheck('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.check('#bin-list [data-bin-sel="%s"]' % ids[2])
    pg.click("#bin-restore-sel")
    check("Restore selected (two)", wait(lambda: find_event("UIT Bin1") and find_event("UIT Bin2") and rows.count() == 1, 8), (rows.count(), [e["name"] for e in events() if "Bin" in e["name"]]))
    pg.click("#bin-restore-all")
    check("Restore all", wait(lambda: find_event("UIT Bin3") and jget("/api/events/deleted") == [] and "Empty" in pg.inner_text("#bin-list"), 10, 0.3))
    check("an empty bin: its buttons are off", pg.is_disabled("#bin-restore-all") and pg.is_disabled("#bin-empty"))
    for i in ids[:2]:
        http(HOME, "/api/events/delete", form={"id": i})
    ui.settings()
    wait(lambda: pg.locator("#bin-list .frow").count() == 2, 10, 0.2)
    pg.click("#bin-empty")
    check("Empty recycle bin asks first", modal_open(pg, "modal-confirm"))
    ui.ask_answer("Cancel")
    check("... Cancel: still there", len(jget("/api/events/deleted")) == 2)
    pg.click("#bin-empty")
    ui.ask_answer("Empty recycle bin")
    check("... emptied", wait(lambda: jget("/api/events/deleted") == [] and "Empty" in pg.inner_text("#bin-list"), 10, 0.3))
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
    check("the handles can't cross", wait(lambda: float(pg.eval_on_selector(lo, "e => e.value")) <= float(pg.eval_on_selector(hi, "e => e.value")), 10))
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
    # the light once the clock is at 42% (a steady output before that is the old level: 0.006, 2026-11-11)
    check("... the clock uses it", wait(lambda: abs(status()["brightnessPct"] - 42) < 1, 10), status()["brightnessPct"])
    l42 = light_settled()
    pg.fill("#cfg-bright-n", "65")
    check("fixed brightness typed", wait_cfg("brightPct", 65) and pg.input_value("#cfg-bright-s") == "65")
    check("... the clock uses it", wait(lambda: abs(status()["brightnessPct"] - 65) < 1, 10), status()["brightnessPct"])
    l65 = light_settled()
    check("... to the light: the backlight output brighter at 65% than at 42%, both on", 0 < l42 < l65, "output %.3f at 42%%, %.3f at 65%%" % (l42, l65))
    pg.click("#cfg-use-sensor + .sl")
    wait_cfg("useLightSensor", True)
    # Room light -> the light (sensor on; faintest 5%, brightest 90% from above)
    lux0 = clock_state().get("lux", 100)
    ctl("/light?lux=2")
    dark = light_settled(20)
    ctl("/light?lux=800")
    bright = light_settled(20)
    ctl("/light?lux=%g" % lux0)
    check("room light: a dark room -> a faint backlight, a light one -> bright", dark < l42 and bright > dark,
          "output %.3f at 2 lux, %.3f at 800 lux" % (dark, bright))
    pg.click("#rot-scroll")
    check("Single Scroll", wait_cfg("rotationMode", 1) and pg.is_disabled("#cfg-dwell-s"))
    pg.click("#rot-alt")
    check("Alternating (dwell time usable again)", wait_cfg("rotationMode", 0) and not pg.is_disabled("#cfg-dwell-s"))
    rng = jget("/api/limits")["ranges"]   # the sliders' ends: the clock's (the numbers run moved them)
    dwell, speed = rng["messageDwellSec"][0] + 3, rng["scrollSpeed"][0] + 1
    press_range(pg, "#cfg-dwell-s", ["Home"] + ["ArrowRight"] * 3)
    check("dwell time", wait_cfg("messageDwellSec", dwell) and pg.inner_text("#cfg-dwell-lbl") == "%d s" % dwell, pg.inner_text("#cfg-dwell-lbl"))
    press_range(pg, "#cfg-scroll-s", ["Home"] + ["ArrowRight"])
    speed_lbl = "%d/%d" % (speed, rng["scrollSpeed"][1])
    check("scroll speed", wait_cfg("scrollSpeed", speed) and pg.inner_text("#cfg-scroll-lbl") == speed_lbl, pg.inner_text("#cfg-scroll-lbl"))
    pg.wait_for_timeout(2500)
    ui.goto(HOME + "/")
    ui.settings()
    check("after a reload the page shows what the clock has", pg.inner_text("#cfg-scroll-lbl") == speed_lbl and pg.inner_text("#cfg-dwell-lbl") == "%d s" % dwell
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
            if not wait(lambda: cfg()["contentFrequency"].get(k) == int(v), 15):   # (a full run's load: once 6 s+)
                bad.append((k, v, cfg()["contentFrequency"].get(k)))
    check("every row: every frequency (Off .. Every 4th) saved", not bad, bad)
    bad = []
    for cfgkey, values in [("omerMode", [4, 3, 2, 1]), ("nusach", [1, 0]), ("prayerSwitchDays", [0, 2, 1]), ("eventDayChange", [1, 0])]:
        for v in values:
            pg.select_option('#content-order-list select[data-cfg="%s"]' % cfgkey, str(v))
            if not wait_cfg(cfgkey, v, 6):
                bad.append((cfgkey, v, cfg().get(cfgkey)))
    check("every row's own choices saved", not bad, bad)
    drag = lambda key, onto: drag_content_row(pg, key, onto)
    first, third = keys[0], keys[2]
    drag(first, third)
    new = pg.evaluate("currentContentOrder()")
    check("dragged to another place", new == keys[1:3] + [first] + keys[3:], new[:4])
    check("... saved in that order", wait(lambda: cfg()["contentOrder"] == new, 6), cfg()["contentOrder"][:4])
    # back
    drag(first, new[0])
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
    check("Pictures: the clock's own, each with Replace", wait(lambda: pg.locator("#builtin-grid .builtin-item").count() == len(jget("/api/builtin/list")["pictures"]), 15))   # was a fixed 3000 ms (2026-11-14)
    bl = jget("/api/builtin/list")
    check("Icons: how many replaced; the bar shows their space, of the most they may use",
          pg.inner_text("#bi-room") == "%d of %d replaced" % (sum(1 for p in bl["pictures"] if p.get("custom")), len(bl["pictures"]))
          and ("up to %d KB" % (bl["maxBytes"] // 1024)) in pg.inner_text("#st-ic"), (pg.inner_text("#bi-room"), pg.inner_text("#st-ic")))
    item = builtin_item(pg, "Candle lighting")
    # Cancel / ✕ leave it as it was
    item.locator("button", has_text="Replace").click()
    check("Replace opens the editor", wait(lambda: modal_open(pg, "modal-builtin") and "Candle lighting" in pg.inner_text("#bi-title"), 10))
    check("... Save is off until a picture is chosen", pg.is_disabled("#bi-save"))
    pg.locator("#modal-builtin button", has_text="Cancel").click()
    check("Cancel closes it", not modal_open(pg, "modal-builtin"))
    item.locator("button", has_text="Replace").click()
    pg.wait_for_timeout(300)
    pg.click("#modal-builtin .close-x")
    check("✕ closes it", wait(lambda: not modal_open(pg, "modal-builtin"), 10, 0.2) and not builtin("Candles").get("custom"))
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
    # (the page draws, encodes and uploads it first: a fixed 1.5 s read the old one under load - 2026-11-14)
    p = wait(lambda: (lambda q: q if q.get("customFrames") == 1 else None)(builtin("Candles")), 15) or builtin("Candles")
    check("saved: the clock has it, a still", wait(lambda: p.get("custom") and p.get("customFrames") == 1 and not modal_open(pg, "modal-builtin"), 10), p)
    code, bmp = http(HOME, "/api/builtin/get?name=Candles&which=custom", raw=True)
    check("... the file is the picture", code == 200 and bmp[:2] == b"BM" and int.from_bytes(bmp[18:22], "little") == w0)
    item = builtin_item(pg, "Candle lighting")
    check("the grid says yours, with Original", wait(lambda: "yours" in item.inner_text() and item.locator("button", has_text="Original").count() == 1, 15, 0.2))
    # Original: asks, Cancel keeps it, Restore puts the clock's own back
    item.locator("button", has_text="Original").click()
    check("Original asks first, showing the picture", wait(lambda: modal_open(pg, "modal-confirm") and pg.locator("#cf-body canvas").count() == 1, 10))
    ui.ask_answer("Cancel")
    check("... Cancel: still replaced", builtin("Candles").get("custom"))
    item.locator("button", has_text="Original").click()
    ui.ask_answer("Restore")
    check("... Restore: the clock's own again", wait(lambda: not builtin("Candles").get("custom"), 15))
    # An animated GIF
    builtin_item(pg, "Havdalah").locator("button", has_text="Replace").click()
    choose_file(pg, "#modal-builtin button:has-text('Picture or video')", f("anim.gif"))
    check("a GIF: an animation", wait(lambda: (pg.evaluate("bi.frames ? bi.frames.length : 1") or 1) > 1 and " s · " in pg.inner_text("#bi-size"), 10), pg.inner_text("#bi-size"))
    pg.click("#bi-save")
    # (drawn, encoded and uploaded first - a fixed 1.5 s read the old one under load, 2026-11-16)
    p = wait(lambda: (lambda q: q if q.get("custom") and q.get("customFrames", 1) > 1 else None)(builtin("Havdala")), 15) or builtin("Havdala")
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
    check("⇤ Start here", wait(lambda: pg.inner_text(trim + " .trim-start") == "0:01.1", 10), pg.inner_text(trim + " .trim-start"))
    pg.click(trim + ' button[data-a="+1"]')
    pg.click(trim + ' button[data-a="-0.1"]')
    pg.wait_for_timeout(300)
    pg.click(trim + ' button[data-a="end"]')
    check("End here ⇥", wait(lambda: pg.inner_text(trim + " .trim-end") == "0:02.0" and part() == "0.9 s chosen", 10), (pg.inner_text(trim + " .trim-end"), part()))
    pg.click(trim + ' button[data-a="-1"]')
    pg.click(trim + ' button[data-a="gostart"]')
    check("Go to part start", wait(lambda: abs(vt() - 1.1) < 0.05, 10), vt())
    pg.click(trim + ' button[data-a="goend"]')
    check("Go to part end (its last frame)", wait(lambda: abs(vt() - 1.9) < 0.05, 10), vt())
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
    check("the part read: an animation of its frames", wait(lambda: pg.evaluate("bi.frames ? bi.frames.length : 0") == 9 and not pg.is_disabled("#bi-save"), 12),
          (pg.evaluate("bi.frames ? bi.frames.length : 0"), pg.inner_text("#bi-size")))
    pg.click("#bi-save")
    p = wait(lambda: (lambda q: q if q.get("customFrames") == 9 else None)(builtin("Torah")), 15) or builtin("Torah")
    check("... saved as that animation", wait(lambda: p.get("custom") and p.get("customFrames") == 9, 10), p)
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


def rename_clock(ui, to):
    """Settings' Clock name + Save Wi-Fi, as a person: a new name restarts the clock (WebUI.cpp,
    2026-11-15) and the page follows it by itself. True once the clock is back, synced, and the
    page shows Settings again; the same name: just saved."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    n_up = starts()   # restarts counted by starts(), not by the uptime
    ui.clear_toast()
    pg.fill("#cfg-name", to)
    pg.click("button:has-text('Save Wi-Fi')")
    # Which way it went, as the page shows it: a message ("Saved") = no restart; the restart box = a new name
    box = lambda: pg.inner_text("#data-result") if pg.is_visible("#data-result") else ""
    how = wait(lambda: "restart" if "restarts" in box() else ("saved" if ui.toast() else None), 15, 0.2)
    if how != "restart":
        return how == "saved"
    seen = {"down": False}
    def back():
        try:
            s = status()
        except Exception:
            seen["down"] = True
            return False
        return (starts() > n_up) and s.get("synced")
    ok = wait(back, 120, 1)
    ui.goto(HOME + "/")
    ui.settings()
    return ok


def wait_scan(pg, n, timeout=25):
    return wait(lambda: not pg.is_disabled("#cfg-ssid-pick-%d" % n) and pg.locator("#cfg-ssid-pick-%d option" % n).count() > 1, timeout, 0.5)


@section
def wifi(ui, clock):
    """Wi-Fi: clock name, 24-hour, each network's menu (Connect, Add/Change network, Remove), the network list (every choice, ↻, Other…), password Show/Hide, Save Wi-Fi."""
    pg = ui.page
    w = world()
    w["networks"].append({"ssid": "UIT-Net", "password": "uit5k", "ip": "192.168.77.240", "internet": True, "up": True,
                          "rssi": -50, "channel": 11, "bssid": "5A:1E:00:00:77:01"})
    set_world(w)
    ui.goto(HOME + "/")
    ui.settings()
    c0 = cfg()
    name0 = c0["projectName"]
    # name (a new one restarts the clock - section clock_name follows it to the radio)
    check("Clock name saved (in the clock's form), the clock restarted with it",
          rename_clock(ui, "test clock") and cfg()["projectName"] == "TestClock", cfg()["projectName"])
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
    pg.fill("#cfg-pass-3", "uit5k")   # no minimum: the home network's business (Config.h LIMITS, 2026-11-13)
    said = ui.click_msg("button:has-text('Save Wi-Fi')")
    c = cfg()
    check("Save Wi-Fi: network 3 saved, a short password too (5 characters, like an old WEP key)", c.get("wifiSSID3") == "UIT-Net", said)
    check("... shown as network 3, its fields closed", wait(lambda: pg.inner_text("#cfg-ssid-display-3") == "UIT-Net" and not pg.is_visible("#cfg-ssid-pick-3"), 10))
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
        # done = the clock's own line for this lookup (its run.log, live) - the place may already have
        # been "set, not confirmed" from joining, which passed before the lookup was back (2026-11-20)
        looked = lambda: W().count("placeLookedUp", (clock.dir / "run.log").read_text(encoding="utf-8", errors="replace"), n)
        n_looked = looked()
        pg.click("#place-detect-%d" % n)
        check("Detect %d: looked up, not confirmed" % n, wait(lambda: looked() > n_looked, 40, 1)
              and cfg()["places"][n - 1].get("set") and cfg()["places"][n - 1].get("confirmed") is False, cfg()["places"][n - 1])
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
            # (2026-11-18, from the coverage list) a search in Hebrew, with a space - sent URL-encoded, and in
            # Hebrew: the service finds Hebrew names only when asked in Hebrew (in English: nothing)
            api_post("/api/location/search", {"q": "תל אביב"})
            res = wait(lambda: (lambda r: r if r.get("state") not in ("running", "waiting", None) else None)(jget("/api/location/search-result")), 30, 0.5) or {}
            check("a search in Hebrew with a space ('תל אביב'): found, named in Hebrew (asked with language=he)",
                  "תל אביב" in json.dumps(res, ensure_ascii=False), json.dumps(res, ensure_ascii=False)[:200])
            # a city whose time zone isn't in the clock's table (Casablanca: summer time by Ramadan, no
            # fixed rule - utils/make_tz_table.js leaves it out): its fixed offset, no summer time
            code, said = api_post("/api/location/place", {"slot": 2, "city": "Casablanca", "country": "Morocco", "tzid": "Africa/Casablanca",
                                                           "lat": 33.5731, "lon": -7.5898, "offsetSec": 3600})
            check("a place whose zone isn't in the table (Casablanca): saved, the clock on its fixed offset (+01:00, no summer time)",
                  code == 200 and wait(lambda: status().get("tzRule") == "<+0100>-1", 15, 0.5), (code, said[:80], status().get("tzRule")))
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
    check("Remove: network 3 gone", wait(lambda: cfg().get("wifiSSID3", "") == "" and pg.inner_text("#cfg-ssid-display-3") == "(empty)", 15, 0.3),
          (cfg().get("wifiSSID3"), pg.inner_text("#cfg-ssid-display-3")))
    # Networks 2 and 1: Remove, then put back by hand (Other…, the name and password typed)
    pw = {n["ssid"]: n["password"] for n in world()["networks"]}
    for n in (2, 1):
        key = "wifiSSID" + ("" if n == 1 else str(n))
        ui.dialog_answer[0] = True
        row_menu(pg, n, "cfg-wifi-remove-%d" % n)
        check("Remove network %d" % n, wait(lambda: cfg().get(key, "") == "" and pg.inner_text("#cfg-ssid-display-%d" % n) == "(empty)", 15, 0.3))
        row_menu(pg, n, "cfg-wifi-change-%d" % n)
        wait_scan(pg, n)
        pg.select_option("#cfg-ssid-pick-%d" % n, label="Other…")
        pg.fill("#cfg-ssid-%d" % n, names[n])
        pg.fill("#cfg-pass-%d" % n, pw[names[n]])
        pg.click("button:has-text('Save Wi-Fi')")
        check("... put back by typing it (Other…)", wait(lambda: cfg().get(key) == names[n], 6) and wait(lambda: pg.inner_text("#cfg-ssid-display-%d" % n) == names[n], 6))
    # No Wi-Fi password in any log, in any form (2026-11-17): the clock's saved log and its live
    # output since its start (its settings read at the start - where one was printed - included)
    pws = [p for p in set(pw.values()) | {"uit-pass-1", "uit5k"} if len(p) >= 5]
    logs = http(HOME, "/api/debug/diagnostic-log").decode("utf-8", "replace") + (clock.dir / "run.log").read_text(encoding="utf-8", errors="replace")
    saved = json.loads((clock.fs / "config.json").read_text(encoding="utf-8"))
    enc = [v for k, v in saved.items() if k.startswith("wifiPassword") and isinstance(v, str) and len(v) >= 10]   # the "encrypted" form, as stored
    found = [p for p in pws if p in logs] + [e for e in enc if e in logs]
    check("no Wi-Fi password in the clock's log or its live output (%d passwords, %d stored forms looked for)" % (len(pws), len(enc)),
          not found and len(pws) >= 3 and len(enc) >= 2, len(found))
    # name back
    check("name back", rename_clock(ui, name0) and cfg()["projectName"] == name0, cfg()["projectName"])

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


def xlsx_made_by(path):
    """The clock software version an export names (index.html stampMadeBy), "" = none."""
    import zipfile, re as _re
    m = _re.search(r'<definedName name="JewishClockVersion">"([^"]*)"</definedName>', zipfile.ZipFile(path).read("xl/workbook.xml").decode("utf-8"))
    return m.group(1) if m else ""


def xlsx_restamp(src, dst, version):
    """A copy of an export naming another version (None: none at all - as files made before 2026-11-15)."""
    import zipfile, re as _re
    zi = zipfile.ZipFile(src)
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
        for item in zi.infolist():
            data = zi.read(item.filename)
            if item.filename == "xl/workbook.xml":
                x = data.decode("utf-8")
                x = _re.sub(r'<definedName name="JewishClockVersion">"[^"]*"</definedName>',
                            "" if version is None else '<definedName name="JewishClockVersion">"%s"</definedName>' % version, x)
                data = x.encode("utf-8")
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
    check("Export before choosing: says to choose first", wait(lambda: "Choose Events, Settings or Both first" in pg.inner_text("#data-result"), 10))
    data_button(pg, "Close")
    check("Close closes it", wait(lambda: not pg.is_visible("#data-result"), 10))
    n_events = len(events())
    LIMITS_T = jget("/api/limits")["textBytes"]
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
    check("Cancel: nothing changed (watched 2 s)", stays(lambda: find_event("UIT Imp A") and find_event("UIT Imp B") and not find_event("UIT Imp A2"), 2, 0.5))
    import_file(pg, changed)
    data_button(pg, "Import events")
    said = pg.inner_text("#data-result")
    check("Import events: says what changed (a rename = 1 new + 1 removed)", "1 new" in said and "2 removed" in said and "UIT Imp A2" in said, said[:300])
    check("... and the clock has it so", find_event("UIT Imp A2") and not find_event("UIT Imp A") and not find_event("UIT Imp B"))
    row_c = re.search(r'<row r="(\d+)">(?:(?!</row>).)*UIT Imp C', ev_xml(xlsx_sheets(changed)) or next(x for x in xlsx_sheets(changed).values() if "UIT Imp C" in x), re.S)
    # the dates are checked with the clock after the import's own lines - waited for, not read once
    # (2026-11-18: missing on the counting build's slower clock)
    said = wait(lambda: (lambda t: t if "check these" in t else None)(pg.inner_text("#data-result")), 15, 0.25) or pg.inner_text("#data-result")
    check("... a row whose dates don't match: flagged with its row, both dates and the right one",
          row_c and ("Row %s (UIT Imp C): " % row_c.group(1)) in said and W().has("dateMismatchG", said, "14/3/1990") and "י״ז אדר" in said
          and "כ׳ אדר" in said,
          said[said.find("check these"):][:300])
    data_button(pg, "Undo this import")
    check("Undo: the events as before", wait(lambda: find_event("UIT Imp A") and find_event("UIT Imp B") and not find_event("UIT Imp A2"), 15), pg.inner_text("#data-result")[:200])
    data_button(pg, "Close")
    # A damaged file / not an Excel file
    # Several problems in one file (2026-11-13): every one listed at once - row, column, value
    long_a = "UIT Imp A" + "א" * 70   # 149 bytes
    bad = xlsx_edit(files["events"], OUT / "files" / "bad.xlsx", lambda n, x: x.replace(">yahrzeit<", ">bogus<").replace(">UIT Imp A<", ">" + long_a + "<"))
    said = import_file(pg, bad)
    check("a file with bad rows: nothing can be imported, says which row", PW().has("notOneOf", said) and "birthday" in said and pg.locator("#data-result button", has_text="Import events").count() == 0, said[:200])
    check("... a name too long in the same file: listed too, the name in full, the limit said",
          ("(%s): " % long_a) in said and PW().has("cellTooLong", said, None, PW().text("limitCharsHeb", LIMITS_T["name"], LIMITS_T["name"] // 2)), said[:400])
    data_button(pg, "Cancel")
    # Hebrew month names typed in Excel (2026-11-16): the ways a person writes them are read as the
    # month; a name that isn't one, or Adar II in a year without it, refused with the row named.
    # UIT Imp B is 7 Av 5740 (one Adar that year).
    def month_as(text):
        def fn(name, x):
            return re.sub(r'(<row r="\d+">(?:(?!</row>).)*UIT Imp B(?:(?!</row>).)*?)>אב<', lambda m: m.group(1) + ">" + text + "<", x, flags=re.S)
        return xlsx_edit(files["events"], OUT / "files" / "month.xlsx", fn)
    for typed, want_m, want_a2 in (("אדר א׳", "אדר", False), ("אדר א", "אדר", False), ("מרחשוון", "חשוון", False),
                                   ("חשון", "חשוון", False), ("סיוון", "סיון", False)):
        said = import_file(pg, month_as(typed))
        offered = pg.locator("#data-result button", has_text="Import events").count() == 1
        if offered:
            data_button(pg, "Import events")
        e = wait(lambda: (lambda x: x if x and x.get("jMonth") == want_m else None)(find_event("UIT Imp B")), 15) if offered else None
        check("Excel month typed \"%s\": read as %s" % (typed, want_m), offered and e and e.get("isAdarII", False) == want_a2 and e.get("jD") == 7,
              (said[:200], e))
        if offered:
            data_button(pg, "Undo this import")
            wait(lambda: (find_event("UIT Imp B") or {}).get("jMonth") == "אב", 15)
        data_button(pg, "Close")
    for typed, says in (("אדר ג", ("notMonthName",)), ("Adar", ("notMonthName",)),
                        ("אדר ב׳", ("noAdarII",))):
        said = import_file(pg, month_as(typed))
        check("Excel month typed \"%s\": refused, the row named, nothing offered" % typed,
              (PW() if says[0] in PW().texts else W()).has(says[0], said) and "UIT Imp B" in said and pg.locator("#data-result button", has_text="Import events").count() == 0, said[:300])
        data_button(pg, "Cancel")
    check("... the event as it was", (find_event("UIT Imp B") or {}).get("jMonth") == "אב")
    # The clock itself, asked by any client: a month name it doesn't know is refused, never a date
    # (2026-11-16: a build from before DateRules gave "אדר א׳" 14 5784 as 16 Oct 2024)
    for m in ("אדר א׳", "Adar", ""):
        code, body = api_post("/api/calendar/convert", {"from": "h", "y": 5784, "month": m, "d": 14})
        check("the clock's date conversion: month \"%s\" refused" % m, code == 400 and W().has("notADate", body.decode("utf-8", "replace")), (code, body[:120]))
    code, body = api_post("/api/calendar/convert", {"from": "h", "y": 5784, "month": "אדר", "d": 14, "isAdarII": True})
    check("... and 14 Adar II 5784 = 24 Mar 2024", code == 200 and json.loads(body) == {"y": 2024, "m": 3, "d": 24}, (code, body[:120]))
    long_n = "Uit" + "Long" * 10

    def long_texts(name, x):
        x = re.sub(r'(>projectName</t></is></c><c r="B\d+" t="inlineStr"><is><t xml:space="preserve">)[^<]*', lambda m: m.group(1) + long_n, x)
        return re.sub(r'(>separator</t></is></c><c r="B\d+" t="inlineStr"><is><t xml:space="preserve">)[^<]*', lambda m: m.group(1) + "שלום" * 5, x)
    said = import_file(pg, xlsx_edit(files["settings"], OUT / "files" / "settings_long.xlsx", long_texts))
    check("settings too long in Excel: both named with row, setting and value; nothing offered",
          PW().has("settingsRowTooLong", said, None, "projectName", long_n, PW().text("limitLetters", LIMITS_T["projectName"]))
          and PW().has("settingsRowTooLong", said, None, "separator", "שלום" * 5,
                       PW().text("limitCharsHeb", LIMITS_T["separator"], LIMITS_T["separator"] // 2))
          and pg.locator("#data-result button", has_text="Import settings").count() == 0, said[:400])
    data_button(pg, "Cancel")
    said = import_file(pg, f("notapic.txt"))
    check("not an Excel file: Nothing was changed", PW().has("nothingChanged", said) and PW().has("notExported", said), said[:200])
    data_button(pg, "Close")
    # Which version made the file (2026-11-15, VersionRule.h): this version reads its own major's
    # and the one before; newer or older than that - refused with the reason, nothing changed.
    fw = status()["firmware"]
    major = int(fw.split(".")[0])
    check("every export names the version that made it", all(xlsx_made_by(v) == fw for v in files.values() if v),
          {k: xlsx_made_by(v) for k, v in files.items() if v})
    before = sorted(e["name"] for e in events())
    for label, ver, says in (("newer", "%d.0.0" % (major + 1), ("verNewer", None, "%d.0.0" % (major + 1), fw)),
                             ("two majors older", "%d.0.0" % (major - 2), ("verTooOld", None, "%d.0.0" % (major - 2), fw, major - 1))):
        said = import_file(pg, xlsx_restamp(files["both"], OUT / "files" / ("made_by_%s.xlsx" % label.replace(" ", "_")), ver))
        check("a file made by a %s version (%s): refused with the reason, nothing offered" % (label, ver),
              W().has(says[0], said, *says[1:]) and PW().has("nothingChanged", said) and pg.locator("#data-result button", has_text="Import").count() == 0, said[:300])
        data_button(pg, "Close")
        code, body = http(HOME, "/api/events/import?madeBy=" + ver, form={"body": "[]"}, raw=True)
        check("... the clock itself refuses it too (import, %s)" % label, code == 400 and W().has(says[0], body.decode("utf-8", "replace"), *says[1:]), (code, body[:200]))
        code, body = api_post("/api/restore", {"version": ver, "config": {"projectName": "UitNever"}})
        check("... and a restore naming it (%s)" % label, code == 400 and W().has(says[0], body.decode("utf-8", "replace"), *says[1:]) and cfg()["projectName"] != "UitNever", (code, body[:200]))
    check("... events unchanged", sorted(e["name"] for e in events()) == before)
    said = import_file(pg, xlsx_restamp(files["events"], OUT / "files" / "made_by_none.xlsx", None))
    check("a file naming no version (exported before 2026-11-15): offered as always", pg.locator("#data-result button", has_text="Import events").count() == 1, said[:200])
    data_button(pg, "Cancel")
    said = import_file(pg, xlsx_restamp(files["events"], OUT / "files" / "made_by_prev.xlsx", "%d.9.9" % (major - 1)))
    check("a file of the previous major version (%d.9.9): offered" % (major - 1), pg.locator("#data-result button", has_text="Import events").count() == 1, said[:200])
    data_button(pg, "Cancel")
    # Settings: a changed setting comes in; the clock restarts, the page reloads by itself
    def set_scroll(name, x):
        return re.sub(r'(<t xml:space="preserve">scrollSpeed</t></is></c><c r="B\d+"><v>)\d+(</v>)', r"\g<1>7\2", x)
    sfile = xlsx_edit(files["settings"], OUT / "files" / "settings7.xlsx", set_scroll)
    said = import_file(pg, sfile)
    check("a settings file: names its Wi-Fi, offers Import settings", "settings" in said and "Wi-Fi" in said and pg.locator("#data-result button", has_text="Import settings").count() == 1, said[:200])
    n0 = starts()
    data_button(pg, "Import settings")
    check("Import settings: the clock restarts", wait(lambda: restarted_since(n0), 90, 1))
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
    """Clock software: version, Quick start guide, Manual, Licenses, Update the clock, Get log, Web answers > Record."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    v = status()["firmware"]
    check("the version shown", v in pg.inner_text("#fwupd-version"), pg.inner_text("#fwupd-version"))
    # The clock's own Wi-Fi password is a setting (2026-11-14): this build's compiled-in one is
    # written into the settings and stays - as the family's clock will do with its next update
    saved = wait(lambda: json.loads((clock.fs / "config.json").read_text(encoding="utf-8")).get("apPassword"), 15, 0.5)
    check("the clock's own Wi-Fi password: in its settings file, the one it uses", saved and saved == cfg().get("apPassword"),
          (saved, cfg().get("apPassword")))
    # An update that went back by itself (Updater::failedUpdate(), 2026-11-14): said under the version
    (clock.fs / "update_failed.txt").write_text("9.9.9" + chr(10), encoding="utf-8")
    shown = wait(lambda: pg.is_visible("#fwupd-failed") and "update to 9.9.9 failed" in pg.inner_text("#fwupd-failed").lower(), 10, 0.5)
    check("an update that went back by itself: said under the version", shown and status().get("updateFailed") == "9.9.9",
          (pg.inner_text("#fwupd-failed") if pg.is_visible("#fwupd-failed") else "hidden", status().get("updateFailed")))
    ui.clear_toast()
    pg.click("#fwupd-failed-ok")
    check("... OK, seen: gone from Settings, the clock, the status - until another update fails",
          wait(lambda: not pg.is_visible("#fwupd-failed-row"), 10, 0.5) and status().get("updateFailed") == ""
          and not (clock.fs / "update_failed.txt").exists(), status().get("updateFailed"))
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
          "== Status ==" in text and "== Settings ==" in text and "== Diagnostic log ==" in text and "uit5k" not in text, path)
    # Web answers > Record (2026-11-14): every request of the list, one file; imported, the
    # simulator's comparison against this same clock finds nothing different
    path = download(pg, "#rec-btn", timeout=120000)
    rec = json.loads(path.read_text(encoding="utf-8")) if path else {}
    reqs = rec.get("requests", [])
    lst = json.loads(pg.evaluate("JSON.stringify(RECORD_REQUESTS)"))
    by = {(r["method"], r["path"]): r for r in reqs if r.get("accept") == "browser"}
    check("Record: one file, an answer to every request of the list, none failed",
          path is not None and path.name.startswith("JewishClock-answers-") and rec.get("via") == "browser"
          and all(("GET", g) in by for g in lst["gets"]) and all(("POST", q[0]) in by for q in lst["posts"])
          and not any("error" in r for r in reqs), (path, len(reqs), [r for r in reqs if "error" in r][:2]))
    check("... the clock's own answers: status 200, a refused POST 400, the page files' quick 304",
          by.get(("GET", "/api/status"), {}).get("status") == 200 and by.get(("POST", "/api/events/upsert"), {}).get("status") == 400
          and any(r.get("accept") == "browser+If-None-Match" and r.get("status") == 304 for r in reqs),
          [(r["path"], r.get("status")) for r in reqs][:6])
    ref = OUT / "files" / "answers_ref"
    imp = subprocess.run([sys.executable, str(ROOT / "sim" / "import_answers.py"), str(path), "--out", str(ref)], capture_output=True,
                         text=True, encoding="utf-8") if path else None
    cmp_ = subprocess.run([sys.executable, str(ROOT / "sim" / "compare_web.py"), "--sim", HOME, "--ref", str(ref)], capture_output=True,
                          text=True, encoding="utf-8") if imp and imp.returncode == 0 else None
    last = (cmp_.stdout.strip().splitlines() or [""])[-1] if cmp_ else (imp.stderr if imp else "")
    check("... imported (sim/import_answers.py) and compared with this clock (sim/compare_web.py): nothing different",
          cmp_ is not None and cmp_.returncode == 0 and ", 0 different" in last, (last, cmp_.stdout[-800:] if cmp_ else ""))
    pg.click("a:has-text('Update the clock')")
    pg.wait_for_load_state()
    check("Update the clock: the update page", pg.url.endswith("/update.html"), pg.url)


def _online_update(ui, clock, part):
    """Updates from the internet (OnlineUpdate.h) against a stand-in for GitHub's release feed on
    this PC (its download link redirects, as GitHub's does). Every way a feed or a download can be
    wrong (2026-11-15, the user: "just new is not enough"): the feed failing or unreadable, nothing
    newer, a release not ready (draft, package still arriving, no size), downloads failing, cut
    short, too long, damaged, signed with another key, versions too old / too new / just new
    enough; then Install with the key, and the night's automatic install without a key (none)
    and with it."""
    # Three sections since 2026-11-15 (one was 11 of a run's minutes, on one clock): the
    # feed / the download / install and the night - each its own clock when in different lanes.
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer   # not "import http.server": it hides http()
    import struct
    import tempfile
    import threading
    sys.path.insert(0, str(ROOT / "utils"))
    import update_key as uk
    pg = ui.page
    pkg, pv = make_package()
    if not check("a package of this build to offer", pkg is not None, pkg):
        return
    data = pkg.read_bytes()
    major = int(pv.split(".")[0])
    body0 = data[:data.rindex(b"JCSIG1")]

    def signed(body, key=None):
        sig = uk.sign(body, key) if key else uk.sign(body)
        return body + b"JCSIG1" + struct.pack("<H", len(sig)) + sig

    def as_version(v):
        """This build's package, saying it is version v - signed again with the real key."""
        n = body0[6]
        return signed(b"JCUPD1" + bytes([len(v)]) + v.encode() + body0[7 + n:])

    with tempfile.TemporaryDirectory() as d:
        other = Path(d) / "other.pem"
        subprocess.run([uk.openssl(), "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(other)], check=True)
        other_key = signed(body0, other)
    damaged = bytearray(data); damaged[len(damaged) // 2] ^= 0xFF

    F = {"code": 200, "tag": "v99.0.0", "state": "uploaded", "draft": False, "size": None, "file": data,
         "serve": None, "dl_code": 200, "junk": False, "repo": 200, "hits": 0}
    REPO = "/repos/uit/JewishClock-Updates"   # GitHub's shape: the feed is <repo>/releases/latest
    class Feed(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass
        def do_GET(self):
            port = self.server.server_address[1]
            if self.path == REPO:   # the repository itself (asked after a 404)
                body = json.dumps({"full_name": REPO[7:], "private": False}).encode()
                self.send_response(F["repo"])
                if F["repo"] != 200:
                    self.send_header("Content-Length", "0"); self.end_headers(); return
            elif self.path == REPO + "/releases/latest":
                F["hits"] += 1
                if F["code"] != 200:
                    self.send_response(F["code"]); self.send_header("Content-Length", "0"); self.end_headers(); return
                size = len(F["file"]) if F["size"] is None else F["size"]
                body = b"{not json" if F["junk"] else json.dumps({"tag_name": F["tag"], "draft": F["draft"], "prerelease": False, "assets": [
                    {"name": "notes.pdf", "browser_download_url": "http://127.0.0.1:%d/nope" % port, "size": 1, "state": "uploaded"},
                    {"name": pkg.name, "browser_download_url": "http://127.0.0.1:%d/download" % port, "size": size,
                     "state": F["state"]}]}).encode()
                self.send_response(200); self.send_header("Content-Type", "application/json")
            elif self.path == "/download":
                self.send_response(302); self.send_header("Location", "/files/" + pkg.name); self.end_headers(); return
            elif self.path == "/files/" + pkg.name:
                if F["dl_code"] != 200:
                    self.send_response(F["dl_code"]); self.send_header("Content-Length", "0"); self.end_headers(); return
                body = F["file"] if F["serve"] is None else F["serve"]
                self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
            else:
                self.send_response(404); self.end_headers(); return
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Feed)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    def feed(**k):
        F.update({"code": 200, "tag": "v99.0.0", "state": "uploaded", "draft": False, "size": None, "file": data,
                  "serve": None, "dl_code": 200, "junk": False, "repo": 200})
        F.update(k)

    def online():
        return status().get("online", {})

    def check_now():
        """Check now (the page's button); the clock's answer once it has asked the feed."""
        h = F["hits"]
        pg.locator("#online-check").scroll_into_view_if_needed()
        wait(lambda: not pg.is_disabled("#online-check"), 30, 0.3)
        pg.click("#online-check")
        wait(lambda: F["hits"] > h and online().get("state") == "idle", 30, 0.3)
        return online()

    def log_text():
        return http(HOME, "/api/debug/diagnostic-log").decode("utf-8", "replace")

    seen = {"starts": 0}
    def watch_restart():
        seen.update(starts=starts())
    def restarted():
        try:
            return starts() > seen["starts"] and status()["updateState"] == "ok"
        except Exception:
            return False

    def install_refused(name, want):
        """Install pressed: the clock downloads, refuses, says why - and stays as it was."""
        watch_restart()
        pg.locator("#online-install").scroll_into_view_if_needed()
        pg.click("#online-install")
        # want: (the clock's message name, its filled parts...) - Texts.h
        said = wait(lambda: online().get("state") == "idle" and W().has(want[0], online().get("message", ""), *want[1:]), 120, 0.5)
        check("%s: refused - '%s'" % (name, want[0]), said, online().get("message"))
        check("... the clock unchanged, no restart (watched 3 s)", stays(lambda: not restarted() and status()["firmware"] == pv
              and status()["updateState"] == "ok", 3, 0.5), (status()["firmware"], status()["updateState"]))
        # a check after it (the clock's own, 3 min after a start, came 1 ms later once - 2026-11-20):
        # the version still offered, and why the last try failed still said
        o = check_now()
        check("... a check after it: still offered, the reason still said", o.get("available") and W().has(want[0], o.get("message", ""), *want[1:]), o)
        ui.goto(HOME + "/")   # the next case starts from a fresh page
        ui.settings()

    try:
        api_post("/api/config", {"updateFeed": "http://127.0.0.1:%d%s/releases/latest" % (srv.server_address[1], REPO), "autoUpdate": False})
        ui.goto(HOME + "/")
        ui.settings()
        if part == "feed":
            # ── the feed ──
            # A failed check changes nothing about what the clock knew: another section in this lane
            # may have left a version offered (random order, 2026-11-20) - still offered, Install still works
            known = online().get("available", "")
            feed(code=500)
            o = check_now()
            check("feed answers 500: said, what was known before unchanged (offered: %r)" % known,
                  W().text("olCouldNotAsk", W().text("olAnswerCode", 500)) in o.get("message", "") and o.get("available", "") == known
                  and wait(lambda: W().text("olAnswerCode", 500) in pg.inner_text("#online-state"), 10, 0.3), o)
            # GitHub's 404 means "no release yet" or "no such repository" - the clock asks the
            # repository itself and says which (the user, 2026-11-15)
            feed(code=404)
            o = check_now()
            check("feed 404, its repository there (no release yet - the real feed today): said so, nothing offered",
                  o.get("message") == W().text("olNoVersions") and not o.get("available") and o.get("lastCheck"), o)
            feed(code=404, repo=404)
            o = check_now()
            check("feed 404, its repository not there either: 'The update feed is not there', with the address",
                  W().has("olFeedMissing", o.get("message", "")) and REPO in o.get("message", ""), o)
            # ... and that is said (the user, 2026-11-15): the log and Settings at once; the clock's screen
            # once it has lasted 3 days ("Error - see Settings"), until the feed is back
            today = time.strftime("%Y-%m-%d")
            check("... said at once: the day it was first found (status), the log",
                  o.get("feedMissingSince") == today and W().has("olFeedMissing", log_text()), o.get("feedMissingSince"))
            check("... at the top of Settings, in red, saying to tell whoever maintains the clock",
                  wait(lambda: pg.is_visible("#clock-problems") and "update feed is not there since " + today in pg.inner_text("#clock-problems")
                       and "maintains" in pg.inner_text("#clock-problems"), 15, 0.5), pg.inner_text("#clock-problems") if pg.is_visible("#clock-problems") else "hidden")
            ERR = screen_text(jget("/api/texts"), "errorSeeSettings", missing=lambda n: check("the clock lists a screen text %r (/api/texts)" % n, False))
            check("... not on the clock's screen yet (watched 2 rounds)", never_rounds(lambda t: ERR in t, 2), screen()[:100])
            # N days on: the remembered day set back, the clock restarted (it keeps the day over a restart)
            days = jget("/api/tunables")["FEED_MISSING_SCREEN_AFTER_DAYS"]   # the clock's own - asked while it runs
            clock.stop()
            day = int((clock.fs / "feed_missing.txt").read_text(encoding="utf-8").splitlines()[0].strip())
            (clock.fs / "feed_missing.txt").write_text("%d\n0\n" % (day - days), encoding="utf-8")
            # ... and a second problem at the same time: a text too long in its settings file
            cf = clock.fs / "config.json"
            c = json.loads(cf.read_text(encoding="utf-8"))
            sep0 = c.get("separator", " • ")
            c["separator"] = "שלום" * 5
            cf.write_text(json.dumps(c, ensure_ascii=False), encoding="utf-8")
            clock.launch()
            wait(synced, 90, 1)
            shown, seen_s = screen_shows(lambda t: ERR in t, 240)
            check("... %d days on (kept over a restart): '%s' on the clock's screen" % (days, ERR), shown, seen_s[-3:])
            # "OK, seen" for each problem on its own (the user, 2026-11-15): the screen's line goes only
            # when every one is seen or put right; Settings lists each until put right
            ui.goto(HOME + "/")
            ui.settings()
            check("Settings: both problems, each with its own 'OK, seen'",
                  wait(lambda: pg.is_visible("#seen-feed") and pg.is_visible("#seen-texts"), 15, 0.5))
            pg.click("#seen-feed")
            check("... 'OK, seen' for the feed: said so, still listed", wait(lambda: not pg.is_visible("#seen-feed")
                  and "Seen - not on the clock's screen" in pg.inner_text("#clock-problems") and status()["online"].get("feedMissingSeen") is True, 15, 0.5))
            shown2, _ = screen_shows(lambda t: ERR in t, 240)
            check("... the screen still says '%s' (the texts are not seen yet)" % ERR, shown2)
            pg.click("#seen-texts")
            check("... 'OK, seen' for the texts too: said so, still listed", wait(lambda: not pg.is_visible("#seen-texts")
                  and "Seen - not on the clock's screen" in pg.inner_text("#kept-texts") and cfg().get("textNotesSeen") is True, 15, 0.5))
            check("... every problem seen: the screen's line gone (watched 2 rounds)", never_rounds(lambda t: ERR in t, 2), screen()[:100])
            clock.stop()
            clock.launch()
            check("... kept over a restart (seen stays seen)", wait(synced, 90, 1) and never_rounds(lambda t: ERR in t, 2), screen()[:100])
            api_post("/api/config", {"separator": sep0})
            ui.goto(HOME + "/")
            ui.settings()
            feed(code=404, repo=200)
            o = check_now()
            check("... the feed back: the problem gone from the status and Settings",
                  not o.get("feedMissingSince") and wait(lambda: not pg.is_visible("#clock-problems"), 15, 0.5), o)
            check("... and from the clock's screen (watched 2 rounds)", never_rounds(lambda t: ERR in t, 2), screen()[:100])
            check("... and the file is gone", not (clock.fs / "feed_missing.txt").exists())
            feed(code=404, repo=503)
            o = check_now()
            check("feed 404, its repository's answer an error: could not ask", o.get("message") == W().text("olCouldNotAsk", W().text("olAnswerCode", 503)), o)
            known = online().get("available", "")
            feed(junk=True)
            o = check_now()
            check("feed answer not JSON: said, what was known before unchanged", o.get("message", "") == W().text("olNotReadable") and o.get("available", "") == known, o)
            # The real feed (Config.h UPDATE_FEED_URL, GitHub), asked once - read only: it answers and
            # the clock understands it, whatever it holds (no release yet / up to date / a newer one).
            # Right after "not readable": a check that didn't happen leaves that message, refused here.
            api_post("/api/config", {"updateFeed": ""})
            pg.locator("#online-check").scroll_into_view_if_needed()
            understood = ("olNoVersions", "olUpToDate", "olAvailable", "olNotReady")   # the clock's messages (Texts.h)
            # (a feed change is in use at once - OnlineUpdate::useFeed(); before, a press right after it
            # still asked the old feed, up to a second)
            pg.click("#online-check")
            real = wait(lambda: online().get("state") == "idle" and any(W().has(w, online().get("message", "")) for w in understood), 60, 1)
            # (a renamed / deleted repository: "The update feed is not there" - not understood here, fails)
            check("the real update feed on GitHub: answers, understood", real, online())
            api_post("/api/config", {"updateFeed": "http://127.0.0.1:%d%s/releases/latest" % (srv.server_address[1], REPO)})
            feed(tag="v" + pv)
            o = check_now()
            check("feed has this same version: up to date", W().has("olUpToDate", o.get("message", "")) and not o.get("available"), o)
            feed(tag="v%d.0.0" % max(major - 1, 0))
            o = check_now()
            check("feed has an older version: up to date, never offered", W().has("olUpToDate", o.get("message", "")) and not o.get("available"), o)
            for name, k, want in [("package on GitHub state 'starter'", {"state": "starter"}, "olBeingPutUp"),
                                  ("package on GitHub state 'open'", {"state": "open"}, "olBeingPutUp"),
                                  ("release marked draft", {"draft": True}, "olDraft"),
                                  ("its package of size 0", {"size": 0}, "olUnknownSize")]:
                feed(**k)
                o = check_now()
                check("newer version, %s: not taken - 'not ready yet', Install not offered" % name,
                      not o.get("available") and W().has("olNotReady", o.get("message", ""), None, W().text(want))
                      and wait(lambda: W().has("olNotReady", pg.inner_text("#online-state")), 10, 0.3) and not pg.is_visible("#online-install"), o)
            check("... and logged", W().has("olNotTaken", log_text(), None, W().text("olBeingPutUp")))
            # The log level, followed to the log (a chain, item 9): Debug in Settings -> the update
            # check's own debug lines are in the log; Info -> a check leaves none
            feed(tag="v" + pv)
            pg.select_option("#cfg-log-level", "debug")
            wait(lambda: status()["logLevel"] == "debug", 5)
            n0 = W().count("olFeedAnswered", log_text())
            check_now()
            check("Log level Debug: the update check's debug lines go into the log",
                  wait(lambda: W().count("olFeedAnswered", log_text()) > n0, 10, 0.5), log_text()[-300:])
            ui.goto(HOME + "/")
            ui.settings()
            pg.select_option("#cfg-log-level", "info")
            wait(lambda: status()["logLevel"] == "info", 5)
            n1 = W().count("olFeedAnswered", log_text())
            check_now()
            # Until the A -> B -> C phone test is over, the update check is an area the clock logs at
            # debug by itself (LogLevel.h Focus, the user 2026-11-15) - its lines come at Info too.
            # When that is removed, this goes back to: Info -> a check leaves none (stays(), 3 s).
            check("... Info: the check's debug lines still come (its update area is at debug by itself, for the phone test)",
                  wait(lambda: W().count("olFeedAnswered", log_text()) > n1, 10, 0.5) and status()["logLevel"] == "info", log_text()[-300:])
            feed()
            o = check_now()
            check("Check now: the newer version found, Install offered",
                  o.get("available") == "99.0.0" and wait(lambda: pg.is_visible("#online-install"), 10, 0.3)
                  and "99.0.0" in pg.inner_text("#online-install"), o)
            # The switch with nothing newer on offer: switched on in the real install hour (the
            # 03:13 run, 2026-11-20) the clock rightly installed 99.0.0 at once and restarted -
            # the night install itself is part "install"
            feed(tag="v1.0.0")
            check_now()
            fw0 = status()["firmware"]
            for want in (True, False):
                pg.click("#cfg-autoupdate + .sl")
                check("Install new versions at night by themselves: %s - saved" % ("off" if not want else "on"), wait_cfg("autoUpdate", want), cfg().get("autoUpdate"))
            check("... nothing installed meanwhile (nothing newer offered)", status()["firmware"] == fw0 and online().get("state") == "idle", online())
        if part == "download":
            # ── the download ──
            # each: the clock's message for it (Texts.h) and its filled parts
            cases = [("download answers 404", {"dl_code": 404}, ("olDownloadFailed", W().text("olAnswerCode", 404))),
                     ("download cut short (half of it)", {"serve": data[:len(data) // 2]}, ("olWrongSize", None, len(data))),
                     ("download longer than the feed says", {"serve": data + b"\0" * 1000}, ("olWrongSize", None, len(data))),
                     ("package cut short, the feed saying that size", {"file": data[:len(data) // 2]}, ("updIncomplete",)),
                     ("package damaged in the middle", {"file": bytes(damaged)}, ("updDamagedFile",)),
                     ("package signed with another key", {"file": other_key}, ("updBadSignature",)),
                     ("package not signed", {"file": body0}, ("updNotSigned",)),
                     ("package of an older version (the feed's tag says newer)", {"file": as_version("%d.0.0" % max(major - 1, 0))}, ("updOlderVersion",)),
                     ("package two major versions ahead", {"file": as_version("%d.0.0" % (major + 2))}, ("updCantReadData",))]
            for name, k, want in cases:
                feed(**k)
                check_now()
                install_refused(name, want)
        if part == "install":
            # ── accepted ──
            # One major version ahead, its highest number: still taken (vN reads v(N-1)'s data). The
            # package only SAYS that version (re-signed) - its firmware is this build, so after the
            # restart its web files are discarded as another version's and the clock says that update
            # "failed": that part is this test's doing, cleared at once.
            near = "%d.99.99" % (major + 1)
            feed(tag="v" + near, file=as_version(near))
            check_now()
            watch_restart()
            pg.click("#online-install")
            took = wait(lambda: online().get("state") in ("restarting",) or restarted(), 120, 0.5)
            check("package one major ahead (%s, from %s): accepted, installed, restarts" % (near, pv), took and wait(restarted, 400, 2),
                  (online(), seen))
            wait(synced, 90, 1)
            api_post("/api/update/failed-seen", {})
            feed()
            ui.goto(HOME + "/")
            ui.settings()
            check_now()
            watch_restart()
            pg.click("#online-install")
            check("Install: downloaded (through the redirect), installed, the clock restarts into it - trial passed",
                  wait(lambda: restarted() and status()["firmware"] == pv, 400, 2), (status().get("firmware"), status().get("updateState"), seen))
            wait(synced, 90, 1)
            check("... it said so in the log", W().has("olDownloadingLog", log_text(), "99.0.0") and W().has("updTrialPassed", log_text()))
            # Debug by itself in each update area, the setting untouched (the user, 2026-11-15: the
            # A -> B -> C phone test - nobody at the clock sets anything; Updater::debugFocus)
            lvl = status()["logLevel"]
            trial_over = ("focusBack", lvl, W().text("focusTrialOver"))
            lt = wait(lambda: (lambda t: t if W().has(*trial_over[:1], t, *trial_over[1:]) else None)(log_text()), 120, 2) or log_text()
            # the last of each: earlier installs in this section said the same (the clock's lines, Texts.h)
            order = [W().last_at(n, lt, *f) for n, *f in (("focusOn", W().text("focusDownloading")), ("olDownloadAnswered", "200"),
                                                         ("focusBack", lvl, W().text("focusDownloadOver")),
                                                         ("focusOn", W().text("focusOnTrial")), trial_over)]
            check("... the log at debug by itself while downloading and on trial (setting %s): each start and end said, in order" % lvl,
                  lvl != "debug" and all(i >= 0 for i in order) and order == sorted(order), order)
            check("... the setting itself unchanged (Settings shows it, not the areas' debug)", status()["logLevel"] == lvl, (status()["logLevel"], lvl))
            # ── automatic, at night ──
            # from midday on the clock, whatever the real hour: run at 03:xx it would install the
            # moment it heard of the version, before the watch below (2026-11-20, the night run)
            today = time.strftime("%Y-%m-%d")
            api_post("/api/debug/time", {"local": today + "T12:00"})
            api_post("/api/config", {"autoUpdate": True})
            ui.goto(HOME + "/")
            ui.settings()
            check_now()
            watch_restart()
            hour = jget("/api/tunables")["AUTO_INSTALL_HOUR"]   # the clock's night hour - asked (the numbers run moved it)
            api_post("/api/debug/time", {"local": today + "T%02d:00" % hour})
            check("%02d:00, a newer version, the key: installed by itself, the clock restarts into it" % hour,
                  wait(lambda: restarted() and status()["firmware"] == pv, 400, 2), (online(), seen))
            wait(synced, 90, 1)
            check("... logged as the night's", W().has("olNightInstall", log_text()))
    finally:
        srv.shutdown()
        api_post("/api/debug/time", {"offset": 0})
        api_post("/api/config", {"updateFeed": NO_FEED, "autoUpdate": True})


@section
def online_update(ui, clock):
    """Updates from the internet (OnlineUpdate.h), the feed: failing, unreadable, not there (said at
    once; on the screen after 3 days; 'OK, seen' per problem), the real feed on GitHub, nothing newer,
    a release not ready (draft, package still arriving, no size), the log level, Check now, the switch."""
    _online_update(ui, clock, "feed")


@section
def online_update_download(ui, clock):
    """Updates from the internet, the download: failing, cut short, too long, damaged, signed with
    another key, not signed, versions too old / too new - each refused with its reason, no restart."""
    _online_update(ui, clock, "download")


@section
def online_update_install(ui, clock):
    """Updates from the internet, installed: a version one major ahead, Install (through the redirect,
    trial passed, debug by itself around it), the night's automatic install without a key and with it."""
    _online_update(ui, clock, "install")


@section
def clock_name(ui, clock):
    """The clock's name, followed to what it does (a chain, 2026-11-15): Save Wi-Fi with a new name ->
    the clock restarts with it: its own Wi-Fi's name on the radio, /api/status, the page's title, the
    screen's Wi-Fi line (first minutes after the start); then back. Found here: before, the radio
    kept the old name until some later restart while Settings and the screen said the new one."""
    pg = ui.page
    old = cfg()["projectName"]
    new = "UITClock"
    rename = lambda to: rename_clock(ui, to)

    check("Clock name typed, Save Wi-Fi: saved, the clock restarts with it", rename(new) and cfg()["projectName"] == new, cfg()["projectName"])
    check("... its own Wi-Fi is called that on the radio", wait(lambda: clock_state().get("apSsid") == new, 30, 0.5), clock_state().get("apSsid"))
    check("... /api/status says so", status().get("apSsid") == new, status().get("apSsid"))
    check("... the page came back by itself, its title the new name", wait(lambda: new in pg.title(), 60, 0.5), pg.title())
    wifi_line = screen_text(jget("/api/texts"), "clockWifi", new, missing=lambda n: check("the clock lists a screen text %r (/api/texts)" % n, False))
    shown, seen_s = screen_shows(lambda t: wifi_line in t, 120)   # "Clock's Wi-Fi: <new> - password ..." up to the password
    check("... the screen's Wi-Fi line says the new name - the radio's", shown and clock_state().get("apSsid") == new, seen_s[-2:])
    check("... back to the old name: the clock restarts, the radio too",
          rename(old) and wait(lambda: cfg()["projectName"] == old and clock_state().get("apSsid") == old, 30, 0.5),
          (cfg()["projectName"], clock_state().get("apSsid")))
    n_up = starts()   # restarts counted by starts(), not by the uptime
    check("... the same name saved again: no restart, just 'Saved'", rename(old) and stays(lambda: starts() == n_up, 5, 0.5))
    # A name with Hebrew letters, straight to the clock (not through the page): only whole ASCII letters
    # and digits are kept (StringTools::normalize) - its Wi-Fi name never gets half a letter
    # (2026-11-21: the C library's isalnum() on a plain char was undefined for those bytes)
    n_up = starts()
    api_post("/api/config", {"projectName": "Uit שעון 2"})
    check("a name with Hebrew letters, sent to the clock: kept as its ASCII letters and digits",
          wait(lambda: restarted_since(n_up) and cfg()["projectName"] == "Uit2", 90, 1), cfg().get("projectName"))
    check("... its own Wi-Fi named so, whole letters only", wait(lambda: clock_state().get("apSsid") == "Uit2", 30, 0.5), clock_state().get("apSsid"))
    n_up = starts()
    api_post("/api/config", {"projectName": old})
    wait(lambda: restarted_since(n_up) and cfg()["projectName"] == old, 90, 1)
    wait(synced, 90, 1)


@section
def ap_password_change(ui, clock):
    """The clock's own Wi-Fi password: Settings' Change (2026-11-15) - too short, not plain letters,
    the same one, Cancel; a new one: saved, the clock restarts with it (its own Wi-Fi uses it, the
    setup / status line and Settings say it); back to the old one."""
    pg = ui.page
    lim = jget("/api/limits")
    lo, hi = lim["apPasswordMinChars"], lim["apPasswordMaxChars"]
    old = cfg()["apPassword"]
    btn = "#cf-buttons button >> nth=0"
    ui.goto(HOME + "/")
    ui.settings()
    pg.locator("#cfg-ap-change").scroll_into_view_if_needed()
    pg.click("#cfg-ap-change")
    check("Change: asks for the new password, 'Change and restart' off while empty",
          modal_open(pg, "modal-confirm") and pg.is_visible("#ap-pw-box") and pg.is_disabled(btn))
    pg.fill("#ap-pw-box", "x" * (lo - 1))
    check("... %d characters: said in red, still off" % (lo - 1), pg.is_disabled(btn) and ("%d to %d" % (lo, hi)) in pg.inner_text("#ap-pw-said"),
          pg.inner_text("#ap-pw-said"))
    check("... the box takes at most %d" % hi, pg.get_attribute("#ap-pw-box", "maxlength") == str(hi))
    ui.ask_answer("Cancel")
    check("... Cancel: unchanged", cfg()["apPassword"] == old)
    for label, pw, want in [("Hebrew letters", "סיסמה12345", ("apPwNotChanged", W().text("apPwPlain"))), ("the same one", old, ("apPwSame",))]:
        pg.click("#cfg-ap-change")
        pg.fill("#ap-pw-box", pw)
        ui.clear_toast()
        ui.ask_answer("Change and restart")
        said = ui.wait_toast()
        check("... %s: refused by the clock, saying why" % label, W().has(want[0], said, *want[1:]) and cfg()["apPassword"] == old, said)
    # Every sign of an English keyboard, and a space (the user, 2026-11-15: no angry call about an
    # underscore) - the tricky ones for JSON / the page / the settings file too: " \ ' ` < >
    new = "Aa1 ~!@#$%^&*()_+-=[]{}\\|;:'\",.<>/?`"
    check("... the test password has all 32 signs of a keyboard", sum(1 for c in new if not c.isalnum() and c != " ") == 32, new)
    n_up = starts()   # restarts counted by starts(), not by the uptime
    pg.click("#cfg-ap-change")
    pg.fill("#ap-pw-box", new)
    check("... a good one (%d characters): on" % len(new), not pg.is_disabled(btn) and pg.inner_text("#ap-pw-said") == "%d characters" % len(new))
    ui.ask_answer("Change and restart")
    seen = {"down": False}
    def back():
        try:
            s = status()
        except Exception:
            seen["down"] = True
            return False
        return (starts() > n_up) and s.get("synced")
    check("... saved, the clock restarts with it", wait(back, 120, 1), seen)
    check("... its settings and file have it", cfg()["apPassword"] == new and
          json.loads((clock.fs / "config.json").read_text(encoding="utf-8")).get("apPassword") == new)
    ui.goto(HOME + "/")
    ui.settings()
    check("... Settings shows it after the restart", wait(lambda: new in pg.inner_text("#cfg-ap"), 30, 0.5), pg.inner_text("#cfg-ap"))
    shown, seen_s = screen_shows(lambda t: W().has("clockWifi", t, None, new), 120)
    check("... the clock's screen says it (status slide, first minutes after the start)", shown, seen_s[-3:])
    st = clock_state()
    check("... and its own Wi-Fi (the radio stand-in) uses it", st.get("apPass") == new, st.get("apPass"))
    n_up = starts()   # restarts counted by starts(), not by the uptime
    code, said = api_post("/api/ap-password", {"password": old})
    check("... back to the old one", code == 200, said)
    # Its restart waited for, not just the answer: the next section met a clock half-way through
    # it once (connection refused - 2026-11-15)
    gone = {"down": False}
    def restarted_back():
        try:
            s = status()
        except Exception:
            gone["down"] = True
            return False
        return (starts() > n_up) and s.get("synced") and cfg()["apPassword"] == old
    check("... the clock restarted with it", wait(restarted_back, 120, 1), gone)


@section
def update_key(ui, clock):
    """The update key (UpdateKeyState.h, 2026-11-17): its id in Settings, no Clear; Paste a key
    replaces it (asks, names the old key, Cancel / not a key / a new key); a package signed with
    another key refused; the right key back -> installed, the firmware writes its key; a setting
    with no usable key -> the firmware's own at the next start."""
    sys.path.insert(0, str(ROOT / "utils"))
    import update_key as uk
    pub = uk.header_public_der()
    want_id = uk.key_id(pub)
    other_pem = OUT / "files" / "other_update_key.pem"   # a second key, made for this run
    other_pem.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([uk.openssl(), "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(other_pem)], check=True, capture_output=True)
    other = uk.public_der(other_pem)
    other_id = uk.key_id(other)
    pg = ui.page
    pkg, pv = make_package()
    ui.goto(HOME + "/")
    ui.settings()
    check("Settings: the update key's id", wait(lambda: pg.inner_text("#upd-key") == want_id, 10, 0.3) and status().get("updateKeyId") == want_id,
          pg.inner_text("#upd-key"))
    check("... no Clear (no key = nothing installed), Paste a key there", pg.locator("#upd-key-clear").count() == 0 and pg.is_visible("#upd-key-paste"))
    code, _ = api_post("/api/update/key-clear", {})
    check("... nor a Clear route", code == 404 and status().get("updateKeyId") == want_id, code)
    use_btn = "#cf-buttons button >> nth=0"
    pg.locator("#upd-key-paste").scroll_into_view_if_needed()
    pg.click("#upd-key-paste")
    check("Paste a key: asks for it, names the key it replaces, 'Use this key' off",
          modal_open(pg, "modal-confirm") and pg.is_visible("#upd-key-box") and pg.is_disabled(use_btn) and want_id in pg.inner_text("#modal-confirm"))
    ui.ask_answer("Cancel")
    check("... Cancel: the key stays", status().get("updateKeyId") == want_id)
    pg.click("#upd-key-paste")
    pg.fill("#upd-key-box", "not a key")
    check("... not a key: said, still off", wait(lambda: PW().has("notUpdateKey", pg.inner_text("#upd-key-said")), 10, 0.3) and pg.is_disabled(use_btn),
          pg.inner_text("#upd-key-said"))
    pg.fill("#upd-key-box", " ".join(other.hex()[i:i + 32] for i in range(0, len(other.hex()), 32)))   # as copied: in lines
    check("... a new key (in pieces, as copied): its id, on", wait(lambda: other_id in pg.inner_text("#upd-key-said"), 10, 0.3) and not pg.is_disabled(use_btn),
          pg.inner_text("#upd-key-said"))
    ui.ask_answer("Use this key")
    check("... used: it replaced the old one, Settings shows it", wait(lambda: status().get("updateKeyId") == other_id and pg.inner_text("#upd-key") == other_id, 10, 0.3),
          pg.inner_text("#upd-key"))
    check("... saved in its settings", wait(lambda: json.loads((clock.fs / "config.json").read_text(encoding="utf-8")).get("updateKey") == other.hex(), 15, 0.5))
    code, said = api_post("/api/update/key-set", {"key": "00" * 91})
    check("a wrong key pasted (route): refused, the key unchanged", code == 400 and W().has("notUpdateKey", said.decode("utf-8", "replace")) and status().get("updateKeyId") == other_id, said)
    if pkg:
        ui.goto(HOME + "/update.html")
        check("the update page with a key: no warning", not wait(lambda: pg.is_visible("#nokey"), 3, 0.3))
        pg.set_input_files("#fileInput", str(pkg))
        n0 = starts()
        pg.click("#goBtn")
        # nothing installed = no restart (the test package is this very version: "firmware" says nothing)
        check("... a package signed with the old key: refused, says why, nothing installed",
              wait(lambda: W().has("updBadSignature", pg.inner_text("#msg")), 120, 1) and stays(lambda: starts() == n0, 8, 1),
              pg.inner_text("#msg"))
        code, said = api_post("/api/update/key-set", {"key": pub.hex()})
        check("the right key pasted back (route): set", code == 200 and status().get("updateKeyId") == want_id, said)
        ui.goto(HOME + "/update.html")
        pg.set_input_files("#fileInput", str(pkg))
        pg.click("#goBtn")
        check("... the same package now: installed (Done)", wait(lambda: pg.inner_text("#msg").startswith("Done!"), 400, 2), pg.inner_text("#msg"))
        check("... the firmware's own key in use", wait(lambda: status().get("updateKeyId") == want_id, 30, 1), status().get("updateKeyId"))
        wait(synced, 90, 1)
    else:
        api_post("/api/update/key-set", {"key": pub.hex()})
    # A settings file with no usable key (edited by hand, damaged): the firmware writes its own at its start
    for bad in ("", "not hex at all"):
        clock.stop()
        f = clock.fs / "config.json"
        c = json.loads(f.read_text(encoding="utf-8"))
        c["updateKey"] = bad
        f.write_text(json.dumps(c, ensure_ascii=False), encoding="utf-8")
        clock.launch()
        check("a settings file whose key is %s: the firmware's own key at the start" % (repr(bad) if bad else "empty"),
              wait(lambda: status().get("updateKeyId") == want_id, 90, 1)
              and wait(lambda: json.loads(f.read_text(encoding="utf-8")).get("updateKey") == pub.hex(), 30, 1), (wait(status, 2) or {}).get("updateKeyId"))
        wait(synced, 90, 1)
    # An older version (agreed 2026-11-13): refused, saying why; the page then offers
    # "install the older version and erase everything" (not done here - the drill does it)
    old = Path("C:/pio3/jc_clean/dist/2.1.2/JewishClock-update-2.1.2.jcupdate")
    if old.exists():
        data = old.read_bytes()
        body = data[:data.rindex(b"JCSIG1")] if b"JCSIG1" in data[-200:] else data
        sig = uk.sign(body)
        signed = OUT / "files" / "older-2.1.2.jcupdate"
        signed.write_bytes(body + b"JCSIG1" + len(sig).to_bytes(2, "little") + sig)
        ui.goto(HOME + "/update.html")
        pg.set_input_files("#fileInput", str(signed))
        pg.click("#goBtn")
        check("an older version from the update page: refused, says why", wait(lambda: W().has("updOlderVersion", pg.inner_text("#msg")), 120, 1), pg.inner_text("#msg"))
        check("... and offers to install it with everything erased", pg.is_visible("#olderBox"))
        pg.check("#eraseOk")
        check("... (ticked: Update with erase on)", pg.is_checked("#eraseOk"))
        pg.uncheck("#eraseOk")
        check("... nothing changed on the clock", status().get("firmware") != "2.1.2")


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
    today = clear_of_midnight()
    real = clock_local()
    # (the Hebrew ones come from the clock's conversion - after midnight it may be busy
    # fetching the new day's calendar: failed once at 00:04 reading them at once)
    wait(lambda: pg.input_value("#dbg-hy") != "" and pg.input_value("#dbg-hd") != "", 10)
    check("the date fields start at today", pg.input_value("#dbg-g")[:10] == today.isoformat() and pg.input_value("#dbg-hy") != "" and pg.input_value("#dbg-hd") != "",
          (pg.input_value("#dbg-g"), pg.input_value("#dbg-hd"), pg.input_value("#dbg-hm"), pg.input_value("#dbg-hy")))
    # Shift
    pg.fill("#dbg-d", "3")
    pg.fill("#dbg-h", "2")
    pg.fill("#dbg-m", "30")
    said = ui.click_msg("button[onclick='debugShift()']")
    want = 3 * 86400 + 2 * 3600 + 30 * 60
    check("Shift by 3 d 2 h 30 min", cfg()["debugTimeOffset"] == want and PW().has("debugTimeSet", said), (cfg()["debugTimeOffset"], said))
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
    check("a day the month doesn't have: refused, says exactly why, the clock stays", W().has("notADate", said) and W().has("noDay", said, None, 30, W().text("nDays", 29))   # the month's name: the clock's (scrambled too)
          and status()["hebrewDate"] == "ט״ו ניסן תשפ״ז", said)
    pg.fill("#dbg-hd", "10")
    pg.fill("#dbg-hy", "תשפ״ו")
    said = ui.click_msg("button[onclick='debugSetHebrew()']")
    check("a month the year doesn't have (Adar II 5786): refused, says so", W().has("notADate", said) and W().has("noAdarII", said, 5786)
          and status()["hebrewDate"] == "ט״ו ניסן תשפ״ז", said)
    pg.fill("#dbg-hd", "")
    check("no day: says what to type", "Hebrew day" in ui.click_msg("button[onclick='debugSetHebrew()']"))
    pg.fill("#dbg-hd", "5")
    pg.fill("#dbg-hy", "")
    check("no year: says what to type", "Hebrew year" in ui.click_msg("button[onclick='debugSetHebrew()']"))
    pg.fill("#dbg-hd", "5"); pg.select_option("#dbg-hm", "ניסן"); pg.fill("#dbg-hy", "2027")
    pg.click("button[onclick='debugSetHebrew()']")
    check("a Gregorian year in the Hebrew box: 'Did you mean תשפ״ז?'", wait(lambda: modal_open(pg, "modal-confirm") and "Did you mean תשפ״ז" in pg.inner_text("#cf-title"), 10), pg.inner_text("#cf-title"))
    ui.clear_toast()
    ui.ask_answer("Cancel")
    check("... Cancel: says why, nothing set", PW().has("looksGregorian", ui.wait_toast()))
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
    check("Back to real time", cfg()["debugTimeOffset"] == 0 and abs((clock_local() - real).total_seconds()) < 600 and PW().has("debugTimeBack", said), said)


@section
def limits(ui, clock):
    """Limits: the page's sliders and boxes take the clock's (/api/limits); the clock keeps every value inside them."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    pg.wait_for_timeout(1500)
    lim = jget("/api/limits")
    # the tunables tests ask (Tunables.h): served, every name once, each a number or a text
    tun = jget("/api/tunables")
    check("/api/tunables: served, no name twice (%d values)" % len(tun),
          len(tun) >= 20 and "duplicates" not in tun and all(isinstance(v, (int, float, str)) for v in tun.values()),
          tun.get("duplicates"))
    # the screen's system messages (Texts.h): served, each a text, the ones the tests ask for there
    tx = jget("/api/texts")
    asked = ("noInternet", "clockWifi", "noNtp", "updateFailed", "errorSeeSettings", "setupLine", "waitingForTime", "loading")
    check("/api/texts: served, every entry a text, the situations the tests ask for listed (%d texts)" % len(tx),
          tx and all(isinstance(v, str) and v for v in tx.values()) and all(k in tx for k in asked), [k for k in asked if k not in tx])
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
    add_event(name="UIT Limit", hasG=True, gD=1, gM=1, gY=0, warn=w1 * 10)
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
    # Texts (2026-11-13): counted in bytes of UTF-8 like the clock; typing stops at the
    # limit with whole letters; the clock refuses a longer one, never cuts it.
    tb = lim["textBytes"]
    check("text limits from the clock", set(tb) >= {"name", "projectName", "wifiSSID", "wifiPassword", "separator"}, tb)
    long_he = "אבגדה" * 30   # 150 letters, 300 bytes
    ui.clear_toast()
    pg.fill("#ev-name", long_he)
    v = pg.input_value("#ev-name")
    check("event name: typing stops at %d bytes, whole Hebrew letters" % tb["name"], tb["name"] - 1 <= len(v.encode()) <= tb["name"] and long_he.startswith(v)
          and PW().has("limitToast", wait(ui.toast, 3) or "", None, PW().text("limitCharsHeb", tb["name"], tb["name"] // 2)), (len(v), len(v.encode()), ui.toast()))
    pg.locator("#modal-event button", has_text="Cancel").click()
    ev = {"name": "א" * (tb["name"] // 2 + 1), "hasG": True, "gD": 1, "gM": 1, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jYNum": 0,
          "type": "other", "icon": "", "warn": 0, "image": ""}
    code, b = api_post("/api/events/upsert", ev)
    check("an event name of %d bytes: the clock refuses it" % len(ev["name"].encode()), code == 400 and W().has("atMostCharsHeb", b.decode(), tb["name"]) and not find_event(ev["name"]), (code, b[:120]))
    before = [e["id"] for e in events()]
    code, b = api_post("/api/events/import", events() + [ev])
    probs = (json.loads(b) if code == 400 and b.startswith(b"{") else {}).get("problems", [])
    check("... in an import: the whole import refused, its row and name given, nothing changed", code == 400
          and any(p["index"] == len(before) and W().has("tooLong", p["text"], W().text("labelEventName", W().text("labelEvent", len(before) + 1))
                                                       + ' "%s"' % ev["name"]) for p in probs)
          and [e["id"] for e in events()] == before, (code, b[:160]))
    # Dates (2026-11-14, DateRules.h - one place for every way a date comes in): a day
    # no month has is never a date; 29 Feb / Adar II / 30 Cheshvan only in years that have them,
    # or without a year.
    def upsert_(**kw):
        e_ = {"name": "UIT Date", "hasG": False, "gD": 0, "gM": 0, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jYNum": 0,
              "isAdarII": False, "afterSunset": False, "type": "other", "icon": "", "warn": 0, "image": ""}
        e_.update(kw)
        c_, b_ = api_post("/api/events/upsert", e_)
        return c_, b_.decode(errors="replace")
    for kw, ok_, word, what in (
            (dict(hasG=True, gD=30, gM=2), False, ("noDay", None, 30), "30 February, no year"),
            (dict(hasG=True, gD=31, gM=4, gY=2020), False, ("noDay", None, 31), "31 April 2020"),
            (dict(hasG=True, gD=29, gM=2, gY=2025), False, ("febNot29", 2025, 2025), "29 February 2025"),
            (dict(hasG=True, gD=29, gM=2, gY=2024), True, "", "29 February 2024"),
            (dict(hasG=True, gD=29, gM=2), True, "", "29 February, no year"),
            (dict(hasJ=True, jD=10, jMonth="אדר", isAdarII=True, jYNum=5785), False, ("noAdarII", 5785), "Adar II 5785"),
            (dict(hasJ=True, jD=30, jMonth="חשוון", jYNum=5786), False, ("hYearDays", None, 5786, 29, 30), "30 Cheshvan 5786"),
            (dict(hasJ=True, jD=30, jMonth="חשוון"), True, "", "30 Cheshvan, no year"),
            (dict(hasJ=True, jD=10, jMonth="אדר", isAdarII=True, jYNum=5784), True, "", "Adar II 5784"),
            (dict(hasJ=True, jD=10, jMonth="אדר", isAdarII=True), True, "", "Adar II, no year"),
            (dict(hasJ=True, jD=30, jMonth="טבת"), False, ("noDay", None, 30), "30 Tevet")):
        c_, said_ = upsert_(**kw)
        check("event %s: %s" % (what, "saved" if ok_ else "refused, says why"), (c_ == 200) if ok_ else (c_ == 400 and W().has(word[0], said_, *word[1:])), (c_, said_[:160]))
        if c_ == 200:
            for e_ in events():
                if e_["name"] == "UIT Date":
                    api_post("/api/events/delete", {"id": e_["id"]})
    for body_, word in (({"from": "g", "y": 2026, "m": 40, "d": 1}, ("gMonthRange", 40)), ({"from": "g", "y": 2026, "m": 2, "d": 30}, ("noDay", None, 30)),
                        ({"from": "h", "y": 5785, "month": "אדר", "isAdarII": True, "d": 1}, ("noAdarII", 5785)), ({"from": "g", "y": 0, "m": 1, "d": 1}, ("yearFrom",))):
        c_, b_ = api_post("/api/calendar/convert", body_)
        check("conversion of %s: refused, says why" % json.dumps(body_, ensure_ascii=False), c_ == 400 and W().has("notADate", b_.decode()) and W().has(word[0], b_.decode(), *word[1:]), (c_, b_[:120]))
    dec_ = convert(**{"from": "h", "y": 5787, "month": "כסלו", "d": 25})   # a date in December: the month search must end
    check("conversion of 25 Kislev 5787 -> December 2026 (a guard once made December endless)", dec_.get("m") == 12 and dec_.get("y") == 2026, dec_)
    check("conversion of 29 February 2024: done",convert(**{"from": "g", "y": 2024, "m": 2, "d": 29}).get("d") == 20, convert(**{"from": "g", "y": 2024, "m": 2, "d": 29}))
    # The import, and the page's check before it: each problem with its row and the file's columns
    bad_ = {"name": "UIT Feb", "hasG": True, "gD": 30, "gM": 2, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jYNum": 0, "type": "other", "icon": "", "warn": 0, "image": ""}
    before = [e["id"] for e in events()]
    code, b = api_post("/api/events/import", events() + [bad_])
    probs = (json.loads(b) if code == 400 and b.startswith(b"{") else {}).get("problems", [])
    check("an import with 30 February: refused whole, the event's place and the day field named", code == 400
          and any(p["index"] == len(before) and p["field"] == "gD" and W().has("noDay", p["detail"], None, 30) for p in probs)
          and [e["id"] for e in events()] == before, (code, b[:200]))
    code, b = api_post("/api/events/check", [bad_, dict(bad_, gD=29, gY=2025)])
    probs = json.loads(b).get("problems", []) if code == 200 else []
    check("/api/events/check: the same answers, nothing changed", code == 200 and [p["field"] for p in probs] == ["gD", "gD gY"]
          and [e["id"] for e in events()] == before, (code, b[:200]))
    said_ = pg.evaluate("""clockEventProblems({events: [Object.assign({row: 7}, %s)], raw: [{C: 'Feb', D: '30'}],
                                                 headers: {C: 'Gregorian month', D: 'Gregorian day'}})""" % json.dumps(bad_, ensure_ascii=False))
    check("the page's Excel check: the row, the file's own column name, the cell, why",
          len(said_) == 1 and said_[0].startswith('Row 7 (UIT Feb), Gregorian day "30": ')
          and W().has("noDay", said_[0], None, 30, W().text("febDays")), said_)   # the month's name: the clock's
    c0 = cfg()
    for key, val, word in (("projectName", "A" * (tb["projectName"] + 1), W().text("labelClockName")),
                           ("wifiSSID3", "x" * (tb["wifiSSID"] + 1), W().text("labelNetName", 3)),
                           ("wifiPassword3", "p" * (tb["wifiPassword"] + 1), W().text("labelNetPassword", 3)),
                           ("separator", " • " * 6, W().text("labelSeparator"))):
        code, b = api_post("/api/config", {key: val})
        check("settings: %s of %d bytes - refused, says why, nothing changed" % (key, len(val.encode())), code == 400 and W().has("tooLong", b.decode()) and word in b.decode() and cfg() == c0, (code, b[:140]))
    # Wi-Fi names and passwords: plain characters only (the user, 2026-11-21 - "we do not know what the
    # router supports"): Hebrew in either is refused, saying why, nothing changed
    for key, val, label in (("wifiSSID3", "רשת הבית", W().text("labelNetName", 3)), ("wifiPassword3", "סיסמה1234", W().text("labelNetPassword", 3)),
                            ("wifiSSID3", "Café", W().text("labelNetName", 3))):
        change = {key: val} if key == "wifiSSID3" else {"wifiSSID3": "UIT-Plain", key: val}
        code, b = api_post("/api/config", change)
        check("settings: %s %r (not plain characters) - refused, says why, nothing changed" % (key, val),
              code == 400 and W().has("wifiNotPlain", b.decode(), label) and cfg() == c0, (code, b[:160]))
    code, b = api_post("/api/config", {"projectName": "A" * (tb["projectName"] + 1), "separator": " • " * 6})
    check("... two too long in one change: both named in one answer", code == 400 and W().text("labelClockName") in b.decode() and W().text("labelSeparator") in b.decode(), (code, b[:200]))
    code, b = api_post("/api/config", {"wifiSSID3": "UIT-Long", "wifiPassword3": "k" * tb["wifiPassword"]})
    check("a password of exactly %d characters (a 64-digit key): taken" % tb["wifiPassword"], code == 200 and cfg().get("wifiSSID3") == "UIT-Long", (code, b[:100]))
    api_post("/api/config", {"wifiSSID3": c0.get("wifiSSID3", ""), "wifiPassword3": ""})
    exact = "•" * (tb["separator"] // 3) + "*" * (tb["separator"] % 3)   # "•" = 3 bytes: exactly the limit
    code, b = api_post("/api/config", {"separator": exact})
    check("... a separator of exactly %d bytes: taken" % tb["separator"], code == 200 and cfg()["separator"] == exact, (code, cfg()["separator"]))
    api_post("/api/config", {"separator": c0["separator"]})
    # In the page: the clock's name and a Wi-Fi name stop at their limits
    ui.settings()
    pg.fill("#cfg-name", "N" * 50)
    check("the clock's name: typing stops at %d" % tb["projectName"], len(pg.input_value("#cfg-name")) == tb["projectName"], len(pg.input_value("#cfg-name")))
    pg.fill("#cfg-name", c0["projectName"])
    ui.clear_toast()
    # (network 1's box is folded away: typed into as the browser would - same listener)
    typed = "() => { const i = document.getElementById('cfg-pass-1'); i.value = '%s'; i.dispatchEvent(new Event('input', {bubbles: true})); return i.value.length; }"
    n = pg.evaluate(typed % ("p" * 80))
    check("a Wi-Fi password: typing stops at %d, says so" % tb["wifiPassword"], n == tb["wifiPassword"]
          and PW().has("limitToast", wait(ui.toast, 3) or "", None, PW().text("limitChars", tb["wifiPassword"])), (n, ui.toast()))
    pg.evaluate(typed % "")
    boxes = pg.evaluate("() => [...document.querySelectorAll('[data-limit-bytes]')].map(i => i.dataset.limitBytes)")
    check("every text box with a byte limit uses one the clock names", boxes and all(k in tb for k in boxes), boxes)
    # The slides' order from outside (an import, a hand-edited file): every slide once,
    # whatever the list holds (2026-11-14: filled slot by slot, a repeat or a missing
    # name could show one slide twice a round and drop another for good)
    order0 = c0["contentOrder"]
    code, b = api_post("/api/config", {"contentOrder": [order0[3], order0[3], "no-such-slide", order0[0]]})
    got = cfg()["contentOrder"]
    check("a slide order with a repeat and an unknown name: every slide once, the named ones first",
          code == 200 and sorted(got) == sorted(order0) and got[:2] == [order0[3], order0[0]], (code, got))
    api_post("/api/config", {"contentOrder": order0})
    check("... and back", cfg()["contentOrder"] == order0)
    # A restore takes settings only (2026-11-18): events in it -> refused, nothing changed
    bk = jget("/api/backup")
    ev_before = [e["id"] for e in events()]
    code, b = api_post("/api/restore", bk)
    said = b.decode()
    check("a restore with events in it: refused (events come in through the Excel import), nothing changed",
          code == 400 and W().has("restoreSettingsOnly", said) and [e["id"] for e in events()] == ev_before and up(), (code, said[:200]))
    # Settings with a text too long: refused as a whole, said, nothing changed
    bk = {"version": bk.get("version"), "config": dict(bk["config"], separator="שלום" * 5)}
    code, b = api_post("/api/restore", bk)
    said = b.decode()
    check("a backup with a separator too long: refused, said, nothing changed",
          code == 400 and W().has("restoreTooLong", said) and "Separator" in said and cfg()["separator"] == c0["separator"] and up(), (code, said[:300]))
    # Too long in the clock's own file - an older version's, or edited by hand
    # (2026-11-13): kept as far as its field holds, and said in the page until changed
    add_event(name="UIT Cut", hasG=True, gD=1, gM=1)
    clock.stop()
    cf, ef = clock.fs / "config.json", clock.fs / "events.json"
    c, evs = json.loads(cf.read_text(encoding="utf-8")), json.loads(ef.read_text(encoding="utf-8"))
    c["separator"] = "שלום" * 5          # 40 bytes
    long_name = "UitKept" + "Long" * 9    # 43 letters: an older version kept up to 63
    c["projectName"] = long_name
    for e in evs:
        if e["name"] == "UIT Cut":
            e["name"] = "UIT Cut " + long_he   # 308 bytes
            e["gM"] = 40                         # no such month
            e["image"] = "../config"             # not a picture id: a path out of the pictures' folder
            e["hasJ"], e["jD"], e["jM"], e["jYNum"] = True, 10, 13, 5785   # Adar II in a year without one
    cf.write_text(json.dumps(c, ensure_ascii=False), encoding="utf-8")
    ef.write_text(json.dumps(evs, ensure_ascii=False), encoding="utf-8")
    clock.launch()
    check("an old file with longer texts: the clock starts", wait(up, 60, 1))
    sep = cfg()["separator"]
    check("... its separator: what fits its field, whole letters", sep == ("שלום" * 5)[:lim["fieldBytes"]["separator"] // 2], sep)
    notes = {n["key"]: n["text"] for n in cfg().get("textNotes", [])}
    check("... the clock says so: the separator in full, the limit, what it uses meanwhile",
          W().has("tooLong", notes.get("separator", ""), W().text("labelSeparator") + ' "%s"' % ("שלום" * 5),
                  W().text("atMostCharsHeb", tb["separator"], tb["separator"] // 2))
          and W().has("usesMeanwhile", notes.get("separator", ""), sep), notes.get("separator"))
    cut = next((e["name"] for e in events() if e["name"].startswith("UIT Cut ")), "")
    check("... the event's name: what fits, whole letters", ("UIT Cut " + long_he).startswith(cut)
          and lim["fieldBytes"]["name"] - 1 <= len(cut.encode()) <= lim["fieldBytes"]["name"], (len(cut.encode()), cut[-5:]))
    cut_id = next((e["id"] for e in events() if e["name"] == cut), "")
    check("... and said: the name in full, open the event", ("UIT Cut " + long_he[:20]) in notes.get("event:" + cut_id, "")
          and W().has("openToCorrect", notes.get("event:" + cut_id, "")), notes.get("event:" + cut_id))
    cut_ev = next((e for e in events() if e["id"] == cut_id), {})
    all_notes = [n["text"] for n in cfg().get("textNotes", []) if n["key"] == "event:" + cut_id]
    check("... its month 40: said, and taken as not given (the event incomplete, not fed to the date maths)",
          cut_ev.get("gM") == 0 and any(W().has("gMonthRange", t, 40) for t in all_notes), (cut_ev.get("gM"), all_notes))
    check("... its Adar II in 5785 (no leap year): said, the date not taken", cut_ev.get("jD") == 0
          and any(W().has("noAdarII", t, 5785) for t in all_notes), (cut_ev.get("jD"), all_notes))
    check("... its picture id that is a path out of the pictures' folder: not taken", cut_ev.get("image") == "", cut_ev.get("image"))
    # Found with nobody doing anything: the clock's own screen says it too (the user, 2026-11-15)
    TOO_LONG = screen_text(jget("/api/texts"), "errorSeeSettings", missing=lambda n: check("the clock lists a screen text %r (/api/texts)" % n, False))
    shown, seen_s = screen_shows(lambda t: TOO_LONG in t, 120)
    check("... the clock's screen says it: '%s' on the status slide" % TOO_LONG, shown, seen_s[-3:])
    # The clock's name kept longer than the limit (2026-11-13): never shortened by the
    # clock; the page shows it in full, marks it and asks for a shorter one; the
    # clock's own Wi-Fi gets its first 32 letters (more than an SSID can have).
    check("... a clock name of %d letters (from an older version): kept as it is" % len(long_name), cfg()["projectName"] == long_name, cfg()["projectName"])
    check("... its own Wi-Fi named with the first %d" % tb["projectName"], jget("/api/status").get("apSsid") == long_name[:tb["projectName"]], jget("/api/status").get("apSsid"))
    ui.goto(HOME + "/")
    ui.settings()
    note = wait(lambda: pg.is_visible("#kept-texts") and pg.inner_text("#kept-texts"), 10) or ""
    check("... the page shows it in full and asks for a shorter one", long_name in note and W().has("atMostLetters", note, tb["projectName"])
          and "field-missing" in (pg.get_attribute("#cfg-name", "class") or ""), note)
    check("... the page lists the separator and the event too", "Separator" in note and "UIT Cut" in note, note[:300])
    code, b = api_post("/api/config", {"projectName": long_name, "use24Hour": c0["use24Hour"]})
    check("... saving with it unchanged still works (it doesn't block the rest), its note stays", code == 200
          and any(n["key"] == "projectName" for n in cfg()["textNotes"]), (code, b[:120]))
    rename_clock(ui, c0["projectName"])   # a new name: the clock restarts with it
    check("... a shorter one saved: its note gone, the others stay", cfg()["projectName"] == c0["projectName"]
          and wait(lambda: long_name not in pg.inner_text("#kept-texts"), 10) and "Separator" in pg.inner_text("#kept-texts"), pg.inner_text("#kept-texts")[:200])
    api_post("/api/config", {"separator": c0["separator"]})
    remove_test_events()
    check("... the separator saved and the event deleted: no notes left", cfg()["textNotes"] == [], cfg()["textNotes"])
    wait(synced, 90, 1)
    # The rename restarted it: its first minutes show the status slide every round - without that line now
    check("... and the screen no longer says it (watched 2 whole rounds)", never_rounds(lambda t: TOO_LONG in t, 2), screen()[:120])
    remove_test_events()


@section
def advanced(ui, clock):
    """Advanced: NTP server, window calibration (4 numbers, Tear lines, Off/Content/Pattern + live picture), Demo, Freeze screen, Hebcal data, log level, the log."""
    pg = ui.page
    ui.goto(HOME + "/")
    ui.settings()
    c0 = cfg()
    # Time servers (2026-11-13): three boxes; one typed in is checked by the clock first -
    # saved only if it answers; the clock then uses the list (the one in use replaced ->
    # the new first at once: /api/status "ntpServer").
    lim = jget("/api/limits")
    check("three time server boxes, as many as the clock keeps", lim["ntpServerSlots"] == 3 and len(c0["ntpServers"]) == 3, c0["ntpServers"])

    def ntp_type(i, text):
        ui.clear_toast()
        pg.fill("#cfg-ntp%d" % i, text)
        pg.press("#cfg-ntp%d" % i, "Tab")
        return wait(lambda: (lambda t: t if any(w in t for w in ("saved", "Not saved", "needed", "Removed")) else "")(ui.toast()), 30, 0.2) or ""

    said = ntp_type(0, "time.windows.com")
    check("NTP server 1: a real server - checked, saved", PW().has("ntpAnswersSaved", said, "time.windows.com") and cfg()["ntpServers"][0] == "time.windows.com", (said, cfg()["ntpServers"]))
    check("... and the clock uses it at once (the one in use was replaced)", wait(lambda: status().get("ntpServer") == "time.windows.com", 5), status().get("ntpServer"))
    before = cfg()["ntpServers"]
    said = ntp_type(1, "no-such-server.invalid")
    check("NTP server 2: a name with no address - refused, says why, box back", PW().has("ntpNotSaved", said) and W().has("ntpTestNoAddress", said)
          and cfg()["ntpServers"] == before and pg.input_value("#cfg-ntp1") == before[1], (said, cfg()["ntpServers"]))
    said = ntp_type(1, "example.com")
    check("NTP server 2: a computer that is no time server - refused (no answer)", PW().has("ntpNotSaved", said) and W().has("ntpTestNoAnswer", said)
          and cfg()["ntpServers"] == before, (said, cfg()["ntpServers"]))
    said = ntp_type(2, "bad name!")
    check("NTP server 3: not a name - refused at once", W().has("ntpBadServerName", said) and cfg()["ntpServers"] == before, said)
    check("box: at most the clock's length", pg.evaluate("el('cfg-ntp2').maxLength") == lim["ntpServerMaxChars"])
    said = ntp_type(2, "")
    check("NTP server 3 emptied: saved without a check", "Removed" in said and cfg()["ntpServers"][2] == "", (said, cfg()["ntpServers"]))
    ntp_type(1, "")
    said = ntp_type(0, "")
    check("the last one emptied: refused, at least one stays", "needed" in said and cfg()["ntpServers"][0] == "time.windows.com"
          and pg.input_value("#cfg-ntp0") == "time.windows.com", (said, cfg()["ntpServers"]))
    # Drag to reorder (2026-11-14): server 2 dragged onto 1 - saved in the new order, the
    # boxes keep their places (box 1 is still "NTP server 1"), and the new first is used at once.
    api_post("/api/config", {"ntpServers": ["time.windows.com", "time.cloudflare.com", ""]})
    ui.goto(HOME + "/")
    ui.settings()
    nh = lambda i: pg.locator('#ntp-list .ntp-row[data-i="%d"] .drag-handle' % i)
    nr = lambda i: pg.locator('#ntp-list .ntp-row[data-i="%d"]' % i)
    pg.locator("#cfg-ntp1").scroll_into_view_if_needed()
    ui.clear_toast()
    a_, b_ = nh(1).bounding_box(), nr(0).bounding_box()
    x_, y0_, y1_ = a_["x"] + a_["width"] / 2, a_["y"] + a_["height"] / 2, b_["y"] + b_["height"] * 0.25
    pg.mouse.move(x_, y0_); pg.wait_for_timeout(150); pg.mouse.down(); pg.wait_for_timeout(80)
    for k_ in range(1, 13):
        pg.mouse.move(x_, y0_ + (y1_ - y0_) * k_ / 12); pg.wait_for_timeout(16)
    pg.wait_for_timeout(300); pg.mouse.up()
    said = wait(lambda: (lambda t: t if PW().has("ntpOrderSaved", t) else "")(ui.toast()), 10, 0.2) or ""
    check("drag NTP server 2 onto 1: saved in the new order", PW().has("ntpOrderSaved", said) and cfg()["ntpServers"][:2] == ["time.cloudflare.com", "time.windows.com"], (said, cfg()["ntpServers"]))
    check("... the boxes keep their places, the servers moved", pg.input_value("#cfg-ntp0") == "time.cloudflare.com" and pg.input_value("#cfg-ntp1") == "time.windows.com"
          and pg.evaluate("Array.from(el('ntp-list').children).map(r => r.dataset.i).join()") == "0,1,2", pg.evaluate("Array.from(el('ntp-list').children).map(r => r.dataset.i).join()"))
    check("... and the clock uses the new first at once", wait(lambda: status().get("ntpServer") == "time.cloudflare.com", 5), status().get("ntpServer"))
    # A server that fails while the internet works (2026-11-21, the user): "Not recommended" under its box - the
    # clock's reason in the page's words, the day it began, its good days of the clock's number
    bad = "no-such-server.invalid"
    api_post("/api/config", {"ntpServers": [bad, "time.cloudflare.com", ""]})
    note = lambda i: pg.evaluate("el('cfg-ntp%d-note').textContent" % i) if pg.is_visible("#cfg-ntp%d-note" % i) else ""
    tr = wait(lambda: [x for x in status().get("ntpTrouble", []) if x["server"] == bad], 60, 0.5)
    pg.evaluate("loadConfig()")   # the list was changed behind the page's back (as from another window): what a reload shows
    shown = wait(lambda: note(0), 10, 0.5) or ""
    day = pg.evaluate("new Date(%d * 1000).toLocaleDateString()" % tr[0]["since"]) if tr else ""
    check("a time server with no address, the internet working: 'not recommended' under its box",
          bool(tr) and shown == PW().text("ntpNotRecommended", W().text("ntpWhyNoAddress"), day, 0, tr[0]["needDays"]), (tr, shown))
    check("... the other boxes say nothing", not note(1) and not note(2), (note(1), note(2)))
    # (the next one may have been asked within the minute - by the drag above: waited for, RFC 4330 s5)
    check("... and the time from the next one", wait(lambda: status().get("ntpServer") == "time.cloudflare.com", 90), status().get("ntpServer"))
    code, b = api_post("/api/config", {"ntpServers": ["", "", ""]})
    check("the clock itself refuses an empty list (a settings import, not the page)", code == 400 and W().has("ntpNeedOne", b.decode("utf-8", "replace")), (code, b[:100]))
    code, b = api_post("/api/config", {"ntpServers": c0["ntpServers"]})
    check("time servers back as before", code == 200 and cfg()["ntpServers"] == c0["ntpServers"], cfg()["ntpServers"])
    pg.evaluate("loadConfig()")
    check("... the mark gone with the server (no note)", wait(lambda: not note(0) and not status().get("ntpTrouble"), 10, 0.5),
          (note(0), status().get("ntpTrouble")))
    # A refused (DENY kiss-o'-death) server: "Refused by server" note under its box (2026-11-21).
    # Mini DENY server - echoes the client's originate timestamp so the firmware accepts the packet.
    import struct, threading
    deny_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    deny_sock.bind(("127.0.0.1", 0))
    deny_port = deny_sock.getsockname()[1]
    deny_got = [0]
    def _deny_serve():
        deny_sock.settimeout(1.0)
        while True:
            try:
                q, addr = deny_sock.recvfrom(512)
                deny_got[0] += 1
                p = bytes([(4 << 3) | 4, 0, 6, (-20) & 0xFF]) + struct.pack(">II", 0, 0) + b'DENY'
                p += b'\x00' * 8 + (q[40:48] if len(q) >= 48 else b'\x00' * 8) + b'\x00' * 16
                deny_sock.sendto(p, addr)
            except socket.timeout:
                pass
            except OSError:
                return
    threading.Thread(target=_deny_serve, daemon=True).start()
    # asked until the simulator says "ok" (a single call before its control port answered was lost, 2026-11-21)
    check("the DENY server in place (the simulator answered)", wait(lambda: ctl("/ntp-port?port=%d" % deny_port) == b"ok", 60, 0.5))
    api_post("/api/config", {"ntpServers": ["127.0.0.1", c0["ntpServers"][0], ""]})
    pg.evaluate("loadConfig()")
    ctl("/ntp-sync")   # 127.0.0.1 is new - no 60 s SAME_SERVER_MIN_MS wait needed
    tr_refused = wait(lambda: next((x for x in status().get("ntpTrouble", []) if x.get("server") == "127.0.0.1" and x.get("refused")), None), 30, 0.5)
    pg.evaluate("loadConfig()")
    shown_r = wait(lambda: note(0), 10, 0.5) or ""
    day_r = pg.evaluate("new Date(%d * 1000).toLocaleDateString()" % tr_refused["since"]) if tr_refused else ""
    check("a refused (DENY) server: 'refused by server' note under its box",
          bool(tr_refused) and shown_r == PW().text("ntpRefused", tr_refused["why"], day_r), (tr_refused, shown_r, "asked %d times" % deny_got[0]))
    check("... the other boxes say nothing", not note(1) and not note(2), (note(1), note(2)))
    api_post("/api/config", {"ntpServers": c0["ntpServers"]})
    ctl("/ntp-port?port=0")
    deny_sock.close()
    pg.evaluate("loadConfig()")
    check("... the refused mark gone once the server is removed", wait(lambda: not note(0) and not status().get("ntpTrouble"), 10, 0.5),
          (note(0), status().get("ntpTrouble")))
    ui.goto(HOME + "/")
    ui.settings()
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
        # Followed to the panel (2026-11-15): the window the clock sends its frames to
        want = [c0[k] + (step_ if k == key else 0) for k in ("calibX", "calibY", "calibW", "calibH")]
        check("... the clock's frames go to that window on the panel", wait(lambda: sim_state().get("panelWindow") == want, 10, 0.25),
              (sim_state().get("panelWindow"), want))
        pg.fill(sel, str(c0[key]))
        wait_cfg(key, c0[key])
    check("... the window as it was", all(cfg()[k] == c0[k] for k in ("calibX", "calibY", "calibW", "calibH")),
          [(k, cfg()[k], c0[k]) for k in ("calibX", "calibY", "calibW", "calibH")])
    want = [c0[k] for k in ("calibX", "calibY", "calibW", "calibH")]
    check("... and on the panel", wait(lambda: sim_state().get("panelWindow") == want, 10, 0.25), (sim_state().get("panelWindow"), want))
    press_range(pg, "#cfg-cal-tear-s", ["ArrowRight"] * 3)
    check("Tear lines", wait(lambda: cfg()["calibTearUs"] == round(c0["calibTearUs"] / 100) * 100 + 300, 6) and pg.inner_text("#cfg-cal-tear-lbl") == "%.1f" % ((round(c0["calibTearUs"] / 100) + 3) / 10),
          (cfg()["calibTearUs"], pg.inner_text("#cfg-cal-tear-lbl")))
    press_range(pg, "#cfg-cal-tear-s", ["ArrowLeft"] * 3)
    # The live picture: the next one 0.25 s after a picture of a moving screen (scrolling,
    # animation - its X-Screen-Moving), 1 s after a still one (the user, 2026-11-13).
    shots = []   # (time, "req") / (time, moving)
    def on_req(r):
        if "/api/debug/screenshot" in r.url:
            shots.append((time.time(), "req"))
    answered = [0.0]   # when the clock answered the last mode switch
    def on_resp(r):
        if "/api/debug/screenshot" in r.url:
            shots.append((time.time(), r.headers.get("x-screen-moving")))
        if "/api/calibration/" in r.url:
            answered[0] = time.time()
    pg.on("request", on_req)
    pg.on("response", on_resp)
    def pg_wait(cond, secs):   # Playwright delivers request events only while it runs - not in time.sleep()
        end = time.time() + secs
        while not cond() and time.time() < end:
            pg.wait_for_timeout(200)
    def mirror_gaps(since):
        # From the first picture asked after the clock took the new mode: the page asks
        # in the new mode's pace only from then (2026-11-16: the switch took over a second
        # under load, and the old mode's last picture was paired with the new one's ask).
        g = {"0": [], "1": []}
        s = list(shots)
        start = next((i for i, x in enumerate(s) if x[1] == "req" and x[0] >= max(since, answered[0])), len(s))
        s = s[start:]
        for (t0, m), (t1, k) in zip(s, s[1:]):
            if m in g and k == "req":
                g[m].append(round(t1 - t0, 2))
        return g
    for mode in ("content", "pattern", "off"):
        if mode == "content":   # single-scroll: the slides always move
            api_post("/api/config", {"rotationMode": 1})
        pg.click("#cal-" + mode)
        t_mode = time.time()
        pg.wait_for_timeout(2500)
        # (4 pictures a second: read at the moment a new one is decoding, it has no
        # width yet - the browser keeps showing the last one meanwhile; failed once so)
        has_pic = lambda: pg.is_visible("#cal-mirror") and pg.evaluate("el('cal-mirror').naturalWidth") > 0
        if mode != "off":
            pg_wait(has_pic, 3)
        mirror = has_pic()
        check("Calibrate %s (the live picture of the screen %s)" % (mode, "shows" if mode != "off" else "hides"), mirror == (mode != "off") and "active" in pg.get_attribute("#cal-" + mode, "class"))
        if mode == "content":
            pg_wait(lambda: len(mirror_gaps(t_mode + 1)["1"]) >= 3, 10)
            g = mirror_gaps(t_mode + 1)
            mv = pg.evaluate("MIRROR_MOVING_MS") / 1000   # the page's own (index.html)
            check("... scrolling: next picture after ~%.2f s" % mv, len(g["1"]) >= 3 and not g["0"] and all(mv * 0.8 <= x <= mv + 0.35 for x in g["1"]), g)
            api_post("/api/config", {"rotationMode": c0["rotationMode"]})
        if mode == "pattern":   # a still picture (calibration's moving frame doesn't count)
            pg_wait(lambda: len(mirror_gaps(t_mode + 1)["0"]) >= 2, 10)
            g = mirror_gaps(t_mode + 1)
            st = pg.evaluate("MIRROR_STILL_MS") / 1000
            check("... standing still: next picture after ~%.2f s" % st, len(g["0"]) >= 2 and not g["1"] and all(st * 0.95 <= x <= st + 0.4 for x in g["0"]), g)
    n_off = len(shots)
    check("... and none asked for once off", stays(lambda: len(shots) == n_off, 1.5), "%d more" % (len(shots) - n_off))
    pg.remove_listener("request", on_req)
    pg.remove_listener("response", on_resp)
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
    check("Clear cache asks first (No: nothing)", wait(lambda: any("Clear the calendar cache" in m for _, m in ui.dialogs[n0:]), 10))
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
    # its end 45 s ahead: a restart and the first answer took 9 s once (2026-11-21, 5 s was the margin -
    # the level had rightly ended before the check)
    end = int(time.time()) + 45
    c["logLevelUntil"] = end
    cf.write_text(json.dumps(c, ensure_ascii=False), encoding="utf-8")
    clock.launch()
    back = wait(up, 60, 1)
    lvl = status().get("logLevel")
    if time.time() >= end:
        log("   (the restart took past the level's end - 'still warn' can't be checked this time)")
    check("after a restart: still warn (its time not up yet)", back and (lvl == "warn" or time.time() >= end),
          "%s, %.0f s before its end" % (lvl, end - time.time()))
    check("... its time up: back to Info by itself, saved so", wait(lambda: status()["logLevel"] == "info" and saved().get("logLevel") == "info"
          and saved().get("logLevelUntil") == 0, 150, 2), (status().get("logLevel"), saved().get("logLevel")))
    wait(synced, 90, 1)
    ui.goto(HOME + "/")
    ui.settings()
    # always: Debug asks first (the log fills sooner); other levels don't
    pg.select_option("#cfg-log-level", "debug")
    wait(lambda: status()["logLevel"] == "debug", 5)
    pg.select_option("#cfg-log-keep", "always")
    check("Debug always: asks first, saying why", wait(lambda: modal_open(pg, "modal-confirm") and "older lines" in pg.inner_text("#cf-body"), 10))
    ui.ask_answer("Cancel")
    check("... Cancel: not saved", stays(lambda: saved().get("logLevel") == "info" and pg.input_value("#cfg-log-keep") == "0", 1.5), (saved().get("logLevel"), pg.input_value("#cfg-log-keep")))
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
            # The update feed's page: the repository the clock serves (Config.h UPDATE_FEED_REPO), filled
            # in when viewed. Clicked with GitHub answered here - a test never visits the real page.
            ui.goto(HOME + page)
            want = "https://github.com/%s/releases" % jget("/api/tunables")["UPDATE_FEED_REPO"]
            feed = pg.locator("a[data-tunable='UPDATE_FEED_REPO']")
            check("the manual's update feed link: the clock's repository",
                  wait(lambda: feed.get_attribute("href") == want and feed.inner_text() == want[len("https://"):], 10, 0.2),
                  (feed.get_attribute("href"), feed.inner_text()))
            pg.route(want, lambda r: r.fulfill(status=200, content_type="text/html", body="the update feed"))
            feed.click()
            check("... and it opens that page", wait(lambda: pg.url == want, 10, 0.2), pg.url)
            pg.unroute(want)
        backs = pg.locator("a.back")
        for i in range(backs.count()):
            # Back closes the tab when the clock's page opened it (Settings > Manual), else goes to
            # the clock's page. A tab the user opened can't be closed by a script - but the test
            # tool's own tab can: that is what failed now and then (2026-11-14). So, as a person's
            # tab: close() does nothing here; and a tab the page opened must close.
            ui.goto(HOME + page)
            pg.wait_for_load_state("load")
            pg.evaluate("window.close = () => {}")
            backs.nth(i).click()
            check("%s: Back to the clock's page (%d)" % (page, i + 1),
                  wait(lambda: pg.url.rstrip("/") == HOME and pg.is_visible("#view-calendar"), 30, 0.3), pg.url)
            with ui.ctx.expect_page() as np:
                pg.evaluate("window.open(%s)" % json.dumps(page))
            p2 = np.value
            p2.wait_for_load_state("load")
            p2.locator("a.back").nth(i).click()
            check("%s: Back in a tab the clock's page opened closes it (%d)" % (page, i + 1), wait(p2.is_closed, 10, 0.2))


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
    check("Back: the clock's page", wait(lambda: pg.url.rstrip("/") == HOME, 10), pg.url)
    if not pkg or not (Path("C:/pio3/jc_clean/sim/versions") / pv / "jcclock.exe").exists():
        check("an update package of this build (pio run, then sim/build_clock.py: the same version)", False, (pkg, pv))
        return
    ui.goto(HOME + "/update.html")
    n0 = starts()
    with pg.expect_file_chooser() as fc:
        pg.click("#filePick")
    fc.value.set_files(str(pkg))
    pg.click("#goBtn")
    check("a real package: Done, the new version runs", wait(lambda: pg.inner_text("#msg").startswith("Done!"), 400, 2), pg.inner_text("#msg"))
    check("... the clock restarted into it, trial passed", status()["firmware"] == pv and starts() > n0 and status()["updateState"] == "ok", status()["firmware"])
    wait(synced, 90, 1)
    # The built-in page (works even if the web files are broken)
    ui.goto(HOME + "/update-safe")
    pg.set_input_files("input[name=firmware]", f("notapic.txt"))
    pg.click("button[type=submit]")
    check("safe update: a wrong file refused", wait(lambda: "fail" in pg.content().lower() or "not" in pg.content().lower(), 15), pg.inner_text("body")[:200])
    ui.goto(HOME + "/update-safe")
    n0 = starts()
    pg.set_input_files("input[name=firmware]", str(pkg))
    pg.click("button[type=submit]")
    check("safe update: a real package installed (restart, trial ok)", wait(lambda: restarted_since(n0) and status()["updateState"] == "ok" and status()["firmware"] == pv, 400, 2),
          status().get("updateState"))
    wait(synced, 90, 1)


@section
def chains(ui, clock):
    """Whole chains, page to screen: own icon, edited library picture, reminder / day / Hebrew day, recycle bin, a row Off, 24-hour, Single Scroll, a replaced icon, Excel export -> import, settings import - each to what the clock shows; Hebrew tooltips."""
    # Each step checked on its own passed while the whole chain did not (the user,
    # 2026-11-10: an own emoji icon showed in the page, never on the clock) - so
    # these follow one action all the way to what the clock's screen shows.
    # Parts (--only chains.<part>), grouped in stages: the stage's page steps first,
    # then one watch of the screen for all of them (2026-11-11; a watch per check before).
    # Since 2026-11-15 a stage's screen checks ask the clock for its coming rounds (Parts.settle());
    # that those ARE what its screen shows is watched here once, in real time.
    ok, detail = preview_matches_screen()
    check("the clock's coming round (asked) is what its screen then shows, slide by slide", ok, detail)
    pg = ui.page
    ph = jget("/api/limits")["pictureMaxH"]
    P = Parts("chains")
    remove_test_events()

    def new_event_today(name):
        ui.goto(HOME + "/")
        pg.click("button.fab")
        pg.wait_for_timeout(1000)
        pg.fill("#ev-name", name)
        pg.check("#ev-has-g")          # the + button fills in today; the tick says it counts
        pg.select_option("#ev-type", "other")

    page_errors = []
    pg.on("pageerror", lambda e: page_errors.append(str(e)))

    def save_and_get(name):
        msg = ui.click_msg("#modal-event button:has-text('Save')")
        wait(lambda: find_event(name) is not None, 10, 0.3)
        if find_event(name):
            return find_event(name)
        why = pg.evaluate("""async () => { const t0 = Date.now(); try { await persistEvent(); return 'returned after ' + (Date.now() - t0) + ' ms, toast: ' + el('toast').textContent; }
                             catch (e) { return 'threw: ' + e.message + ' ' + (e.stack || '').split('\\n')[1]; } }""") if modal_open(pg) else "dialog closed"
        return {"saveMessage": msg, "pageErrors": page_errors[-3:], "saveAgain": why}

    def with_picture(name, pic_id):
        want = "/images/%s.bmp" % pic_id
        return lambda s: name in s and want in s

    def clock_at(local):
        """The test clock's time: "YYYY-MM-DDTHH:MM" in its zone, or None = real time; waits for that day's calendar."""
        api_post("/api/debug/time", {"local": local} if local else {"offset": 0})
        # (waited for a status field that never existed - always true - and a fixed 3 s, until 2026-11-16)
        wait(lambda: status().get("synced"), 60, 1)
        calendar_settled()

    def bmp_size(b):
        return int.from_bytes(b[18:22], "little"), abs(int.from_bytes(b[22:26], "little", signed=True))

    got = {}   # what a part made, for the parts that need it

    # ── Stage: today (real time) - new events, each with its own kind of icon/picture ──
    today = P.stage("today", lambda: clock_at(None))

    @P.part("own_emoji", today)
    def _():
        new_event_today("UIT Own")
        pg.fill("#ev-icon-custom", "😍x🎉")
        check("own icon: an emoji and more -> the box keeps the one emoji", pg.input_value("#ev-icon-custom") == "😍", pg.input_value("#ev-icon-custom"))
        e = save_and_get("UIT Own")
        pid = got["own"] = e.get("iconImage", "")
        check("own emoji saved with its picture (iconImage)", e.get("icon") == "😍" and len(pid) == 8, e)
        code, bmp = http(HOME, "/api/images/get?id=" + (pid or "x"), raw=True)
        if code == 200:
            (OUT / "own_icon_emoji.bmp").write_bytes(bmp)   # to look at
        w, h = bmp_size(bmp) if code == 200 else (0, 0)
        check("... the picture is on the clock, as tall as its icons (%d)" % ph, code == 200 and h == ph and 0 < w <= ph * 1.2 + 1, (code, w, h))
        P.expect("... and the clock's screen shows that picture with the event, today",
                 lambda s, f=with_picture("UIT Own", pid): f(s) and "😍" not in s)

    @P.part("own_letters", today)
    def _():
        n = jget("/api/limits")["iconMaxChars"]
        new_event_today("UIT Letters")
        pg.fill("#ev-icon-custom", "אבגדה")
        check("own letters: the box keeps %d" % n, pg.input_value("#ev-icon-custom") == "אבגדה"[:n], pg.input_value("#ev-icon-custom"))
        e2 = save_and_get("UIT Letters")
        pid2 = got["letters"] = e2.get("iconImage", "")
        check("own letters saved with their picture", e2.get("icon") == "אבגדה"[:n] and len(pid2) == 8 and pid2 != got.get("own"), e2)
        code, bmp2 = http(HOME, "/api/images/get?id=" + (pid2 or "x"), raw=True)
        if code == 200:
            (OUT / "own_icon_letters.bmp").write_bytes(bmp2)
        P.expect("... shown as that picture on the clock today", with_picture("UIT Letters", pid2))

    @P.part("library_crop", today)
    def _():
        src = pg.evaluate("""async () => {
            const c = document.createElement('canvas'); c.width = PHOTO_W; c.height = PHOTO_H;
            const g = c.getContext('2d');
            g.fillStyle = '#c00000'; g.fillRect(0, 0, PHOTO_W / 2, PHOTO_H);
            g.fillStyle = '#00c000'; g.fillRect(PHOTO_W / 2, 0, PHOTO_W / 2, PHOTO_H);
            return (await uploadImageOnce(encodeBmp([g.getImageData(0, 0, PHOTO_W, PHOTO_H).data], PHOTO_W, PHOTO_H, 0))).id; }""")
        add_event(name="UIT Couple", hasG=True, gD=1, gM=1, gY=2000, image=src)   # the picture belongs to another event
        new_event_today("UIT Crop")
        pg.click("text=Library")
        pg.wait_for_timeout(1500)
        k = [i for i, x in enumerate(jget("/api/images/list")) if x["id"] == src][0]
        pg.locator("#ev-gallery .gal-item").nth(k).click()
        check("library picture: zoom shown for it", wait(lambda: pg.is_visible("#ev-photo-zoom") and not pg.evaluate("photo.changed"), 10))
        press_range(pg, "#ev-photo-zoom", ["ArrowRight"] * 80)   # x2.6: the red half alone fills the frame once moved
        box = pg.locator("#ev-photo-canvas").bounding_box()
        pg.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
        pg.mouse.down()
        pg.mouse.move(box["x"] + box["width"] * 1.8, box["y"] + box["height"] / 2, steps=12)   # past the edge: the page stops at the picture's edge
        pg.mouse.up()   # beside the dialog
        rg = pg.evaluate("""() => { const d = el('ev-photo-canvas').getContext('2d').getImageData(0, 0, PHOTO_W, PHOTO_H).data;
            let r = 0, g = 0; for (let i = 0; i < d.length; i += 4) { if (d[i] > 150) r++; if (d[i + 1] > 150) g++; } return [r, g]; }""")
        check("... zoomed and moved: the preview now shows only the red half", rg[0] > 0 and rg[1] == 0, rg)
        check("... the drag let go beside the dialog keeps it open, and ends there", modal_open(pg)
              and "dragging" not in (pg.get_attribute("#ev-photo-canvas >> xpath=..", "class") or ""))
        e4 = save_and_get("UIT Crop")
        lib1 = {x["id"] for x in jget("/api/images/list")}
        check("... saved as a new picture; the original stays for its own event", e4.get("image") and e4["image"] != src and e4["image"] in lib1 and src in lib1, (e4, src))
        P.expect("... and the clock shows the cropped picture today", with_picture("UIT Crop", e4.get("image", "x")))
        # unchanged: the same picture, no copy
        new_event_today("UIT Same")
        pg.click("text=Library")
        pg.wait_for_timeout(1500)
        pg.locator("#ev-gallery .gal-item").nth(k).click()
        pg.wait_for_timeout(800)
        e5 = save_and_get("UIT Same")
        check("library picture not edited: the same one, no copy", e5.get("image") == src and {x["id"] for x in jget("/api/images/list")} == lib1, e5)

    # ── Stage: today, changed - a preset icon again ──
    today2 = P.stage("today, changed")

    @P.part("back_to_preset", today2, needs=["own_letters"])
    def _():
        ui.goto(HOME + "/")
        pg.evaluate("editEvent(events.findIndex(e => e.name === 'UIT Letters'))")
        pg.wait_for_timeout(800)
        pg.click("#icon-quick-picker .icon-option[data-icon='🎉']")
        save_and_get("UIT Letters")
        wait(lambda: not (find_event("UIT Letters") or {}).get("iconImage"), 10, 0.3)
        e3 = find_event("UIT Letters") or {}
        lib = {x["id"] for x in jget("/api/images/list")}
        check("back to a preset icon: no icon picture, the old one removed from the library",
              not e3.get("iconImage") and got.get("letters") not in lib, (e3, got.get("letters") in lib))
        P.expect("... the clock shows its own Party icon for it", lambda s: "UIT Letters" in s and "/IMG/Party.bmp" in s)

    # ── Stage: Excel export -> delete -> import (today) ──
    excel = P.stage("Excel round trip")

    @P.part("excel_icons", excel, needs=["own_emoji", "back_to_preset"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#scope-events")
        xfile = download(pg, "button[onclick='downloadData()']")
        xml = next((x for n, x in xlsx_sheets(xfile).items() if "UIT Own" in x), "") if xfile else ""
        own_row = re.search(r'<row r="\d+">(?:(?!</row>).)*UIT Own(?:(?!</row>).)*</row>', xml, re.S)
        let_row = re.search(r'<row r="\d+">(?:(?!</row>).)*UIT Letters(?:(?!</row>).)*</row>', xml, re.S)
        check("Excel export: each event's icon in column N (own emoji, a chosen one)",
              own_row and re.search(r'r="N\d+"[^>]*><is><t>😍</t>', own_row.group(0)) and let_row and "🎉" in let_row.group(0) and 'r="N1"' in xml,
              (own_row.group(0)[-160:] if own_row else None))
        old_pic = (find_event("UIT Own") or {}).get("iconImage", "")
        for nm in ("UIT Own", "UIT Letters"):
            e = find_event(nm)
            if e:
                http(HOME, "/api/events/delete", form={"id": e["id"]})
        http(HOME, "/api/events/empty-bin", data=b"")
        check("... deleted, bin emptied: the icon's picture is gone from the clock", old_pic not in {x["id"] for x in jget("/api/images/list")})
        import_file(pg, str(xfile))
        data_button(pg, "Import events")
        wait(lambda: find_event("UIT Own") is not None, 15, 0.5)
        eo, el_ = find_event("UIT Own") or {}, find_event("UIT Letters") or {}
        lib = {x["id"] for x in jget("/api/images/list")}
        check("imported: the own icon is back, drawn again (the same picture)", eo.get("icon") == "😍" and eo.get("iconImage") == old_pic and old_pic in lib,
              (eo.get("icon"), eo.get("iconImage"), old_pic, sorted(lib), http(HOME, "/api/images/get?id=" + old_pic, raw=True)[0],
               pg.inner_text("#data-result")[:300] if pg.is_visible("#data-result") else ""))
        check("... and the chosen icon (not the type's)", el_.get("icon", "").startswith("🎉"), el_)
        P.expect("... the clock shows the own icon again", with_picture("UIT Own", old_pic))
        P.expect("... and the chosen icon", lambda s: "UIT Letters" in s and "/IMG/Party.bmp" in s)

    # ── Stage: a settings file imported - the clock restarts with it, the screen follows ──
    simport = P.stage("settings import")

    @P.part("settings_import", simport)
    def _():
        d = datetime.date.today()
        year = heb_year(convert(**{"from": "g", "y": d.year, "m": d.month, "d": d.day})["y"])
        month = status()["todayHMonth"]   # the clock's own (after nightfall it is tomorrow's date)
        date_slide = lambda s: month in s and "[" not in s and "UIT" not in s   # plain text: "ב׳ כסלו תשפ״ז"
        was = cfg()["showHebrewYear"]
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#scope-settings")
        sfile = download(pg, "button[onclick='downloadData()']")
        # the settings sheet keeps each value as text: <t xml:space="preserve">true</t>
        flip = lambda n, x: re.sub(r'(<t xml:space="preserve">showHebrewYear</t></is></c><c r="B\d+"[^>]*><is><t xml:space="preserve">)(true|false)(</t>)',
                                   lambda m: m.group(1) + ("false" if was else "true") + m.group(3), x)
        changed = xlsx_edit(sfile, OUT / "files" / "settings_year.xlsx", flip)
        n0 = starts()
        import_file(pg, changed)
        data_button(pg, "Import settings")
        check("settings file with the Hebrew year switched: imported, the clock restarts with it",
              wait(lambda: restarted_since(n0), 90, 1) and wait(lambda: cfg()["showHebrewYear"] == (not was), 60, 1), cfg().get("showHebrewYear"))
        wait(synced, 90, 1)
        got["settings_file"], got["year_was"], got["year"], got["month"] = sfile, was, year, month
        if was:
            P.expect_absent("... the clock's date slide without the year (%s)" % year, lambda s: date_slide(s) and year in s)
            P.expect("... the date slide still shows", date_slide)
        else:
            P.expect("... the clock's date slide with the year (%s)" % year, lambda s: date_slide(s) and year in s)

    simport2 = P.stage("settings import, back")

    @P.part("settings_import_back", simport2, needs=["settings_import"])
    def _():
        n0 = starts()
        ui.goto(HOME + "/")
        ui.settings()
        import_file(pg, got["settings_file"])
        data_button(pg, "Import settings")
        check("... the exported file imported again: as before", wait(lambda: restarted_since(n0), 90, 1)
              and wait(lambda: cfg()["showHebrewYear"] == got["year_was"], 60, 1))
        wait(synced, 90, 1)
        year, month = got["year"], got["month"]   # (just after a restart the clock's status has no Hebrew date yet)
        has_year = lambda s: month in s and "[" not in s and "UIT" not in s and year in s
        if got["year_was"]:
            P.expect("... and the date slide has its year again (%s)" % year, has_year)
        else:
            P.expect_absent("... and the date slide is without the year again", has_year)

    # ── Stage: Friday 12 March 2027, 10:00 - three days before an event; parasha, candles ──
    fri = P.stage("Friday 2027-03-12", lambda: clock_at("2027-03-12T10:00"))
    is24 = [None]
    cand = lambda s: "/IMG/Candles.bmp" in s
    t24 = lambda s: cand(s) and re.search(r"\b(1[3-9]|2[0-3]):\d\d\b", s)
    t12 = lambda s: cand(s) and re.search(r"\b([1-9]|1[0-2]):\d\d\b", s) and not re.search(r"\b(1[3-9]|2[0-3]):\d\d\b", s)
    psel = '#content-order-list .content-row[data-type="parasha"] .content-freq'
    pf = [None]

    @P.part("full_event", fri)
    def _():
        new_event_today("UIT Rem")
        pg.select_option("#ev-type", "birthday")
        y0 = pg.input_value("#ev-j-y")   # today's Hebrew year, until the clock has converted 15/3/2000
        pg.fill("#ev-g-d", "15"); pg.select_option("#ev-g-m", "3"); pg.fill("#ev-g-y", "2000")
        # waited for, not a fixed 1.2 s: under a full run's load the conversion came later, and the
        # event was saved with today's Hebrew date - "Date mismatch" (2026-11-18)
        wait(lambda: pg.input_value("#ev-j-y") not in ("", y0), 15, 0.2)
        pg.check("#ev-has-j")
        pg.fill("#ev-warn", "3")
        er = save_and_get("UIT Rem")
        check("event with both dates and a reminder saved", er.get("hasJ") and er.get("hasG") and er.get("warn") == 3, er)
        P.expect("... 3 days before: the clock shows it with its reminder", lambda s: "UIT Rem" in s and "בעוד 3 ימים" in s)

    @P.part("friday_before", fri)
    def _():
        is24[0] = cfg()["use24Hour"]
        P.expect("Friday: the parasha shows (before)", lambda s: "/IMG/Torah.bmp" in s)
        P.expect("candle lighting shown with its time, as set (%s)" % ("24 h" if is24[0] else "12 h"), t24 if is24[0] else t12)

    fri2 = P.stage("Friday: parasha Off, 24-hour switched")

    @P.part("parasha_off", fri2, needs=["friday_before"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pf[0] = pg.input_value(psel)
        pg.select_option(psel, "0")
        check("parasha row Off: saved", wait(lambda: cfg()["contentFrequency"].get("parasha") == 0, 6))
        pg.wait_for_timeout(1000)
        P.expect_absent("parasha Off: the clock stops showing it", lambda s: "/IMG/Torah.bmp" in s)

    @P.part("24_hour", fri2, needs=["friday_before"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#cfg-24h + .sl")
        check("24-hour switched: saved", wait(lambda: cfg()["use24Hour"] != is24[0], 6))
        P.expect("24-hour switched: the candle time on the clock follows (%s)" % ("12 h" if is24[0] else "24 h"), t12 if is24[0] else t24)

    fri3 = P.stage("Friday: back on")

    @P.part("parasha_on", fri3, needs=["parasha_off"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.select_option(psel, pf[0])
        wait(lambda: cfg()["contentFrequency"].get("parasha") == int(pf[0]), 6)
        P.expect("... parasha back on: shown again", lambda s: "/IMG/Torah.bmp" in s)

    @P.part("24_hour_back", fri3, needs=["24_hour"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#cfg-24h + .sl")
        wait(lambda: cfg()["use24Hour"] == is24[0], 6)
        P.expect("... 24-hour back: the candle time as before", t24 if is24[0] else t12)

    fri4 = P.stage("Friday: Single Scroll, Torah icon replaced")

    @P.part("single_scroll", fri4, needs=["friday_before"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#rot-scroll")
        check("Single Scroll: saved", wait(lambda: cfg()["rotationMode"] == 1, 6))
        P.expect("Single Scroll: date and parasha in one scrolling line", lambda s: "Torah.bmp" in s and "2027" in s)

    @P.part("replaced_icon", fri4, needs=["friday_before"])
    def _():
        prob = pg.evaluate("""async () => {
            const w = 70, h = PICTURE_H, c = document.createElement('canvas'); c.width = w; c.height = h;
            const g = c.getContext('2d'); g.fillStyle = '#0040ff'; g.fillRect(0, 0, w, h);
            return await uploadBuiltin('Torah', encodeBmp([g.getImageData(0, 0, w, h).data], w, h, 0)); }""")
        check("Torah icon replaced: saved", not prob, prob)
        P.expect("Torah icon replaced: the clock shows the new one with the parasha", lambda s: "/custom/Torah.bmp" in s)

    # The slide order (Settings' drag), seen in Single Scroll's one line: the Gregorian date
    # dragged above the Hebrew date - the line has it first; dragged back - the Hebrew one first
    def before(s, a, b):
        return a in s and b in s and s.index(a) < s.index(b)
    order_was = []

    fri4b = P.stage("Friday: Single Scroll, the Gregorian date dragged first")

    @P.part("order_dragged", fri4b, needs=["single_scroll"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        order_was[:] = cfg()["contentOrder"]
        drag_content_row(pg, "gregoriandate", "hebrewdate")
        check("slide order: the Gregorian date dragged above the Hebrew date - saved",
              wait(lambda: cfg()["contentOrder"].index("gregoriandate") < cfg()["contentOrder"].index("hebrewdate"), 6), cfg()["contentOrder"])
        P.expect("... the clock's line has the Gregorian date before the Hebrew date", lambda s: before(s, "12 מרץ", "ג׳ אדר ב׳"))

    fri4c = P.stage("Friday: Single Scroll, the order back")

    @P.part("order_back", fri4c, needs=["order_dragged"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        drag_content_row(pg, "hebrewdate", "gregoriandate")
        check("... dragged back: saved as it was", wait(lambda: cfg()["contentOrder"] == order_was, 6), cfg()["contentOrder"])
        P.expect("... the Hebrew date first again", lambda s: before(s, "ג׳ אדר ב׳", "12 מרץ"))

    fri5 = P.stage("Friday: Alternating, Original icon")

    @P.part("alternating", fri5, needs=["single_scroll"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.click("#rot-alt")
        wait(lambda: cfg()["rotationMode"] == 0, 6)
        P.expect("Alternating again: one slide at a time", lambda s: "Torah.bmp" in s and "2027" not in s)

    @P.part("original_icon", fri5, needs=["replaced_icon"])
    def _():
        api_post("/api/builtin/reset?name=Torah", {})
        P.expect("... Original: its own Torah icon again", lambda s: "/IMG/Torah.bmp" in s)

    # ── Item 9 (2026-11-15): the less-used controls, each to the screen and back ──
    # Friday 3 Adar II 5787 = 12 March 2027: "ג׳ אדר ב׳ תשפ״ז", "12 מרץ 2027", parasha, candles.
    HDATE, HDATE_Y, GDATE, GDATE_Y = "ג׳ אדר ב׳", "ג׳ אדר ב׳ תשפ״ז", "12 מרץ", "12 מרץ 2027"
    row = lambda t: '#content-order-list .content-row[data-type="%s"] .content-freq' % t
    was = {}

    def row_set(t, v):
        ui.goto(HOME + "/")
        ui.settings()
        was.setdefault(t, pg.input_value(row(t)))
        pg.select_option(row(t), v)
        return check("%s row set to %s: saved" % (t, "Off" if v == "0" else v),
                     wait(lambda: cfg()["contentFrequency"].get(t) == int(v), 6), cfg()["contentFrequency"].get(t))

    def year_switch(want):
        ui.goto(HOME + "/")
        ui.settings()
        if pg.is_checked("#t-heb-year") != want:
            pg.click("#t-heb-year + .sl")
        return check("Show year %s: saved" % ("on" if want else "off"), wait(lambda: cfg()["showHebrewYear"] == want, 6))

    fri6 = P.stage("Friday: year off, Gregorian date Off")

    @P.part("year_off", fri6, needs=["friday_before"])
    def _():
        year_switch(False)
        P.expect("Show year off: the Hebrew date without its year", lambda s: HDATE in s and "תשפ" not in s)
        P.expect_absent("... never with it", lambda s: HDATE_Y in s)

    @P.part("gregorian_off", fri6, needs=["friday_before"])
    def _():
        row_set("gregoriandate", "0")
        P.expect_absent("Gregorian date row Off: the clock stops showing it", lambda s: GDATE in s)

    fri7 = P.stage("Friday: year on, Gregorian date back")

    @P.part("year_on", fri7, needs=["year_off"])
    def _():
        year_switch(True)
        P.expect("... Show year on: the Hebrew date with its year again", lambda s: HDATE_Y in s)

    @P.part("gregorian_on", fri7, needs=["gregorian_off"])
    def _():
        row_set("gregoriandate", was["gregoriandate"])
        P.expect("... Gregorian date back: shown again, with its year", lambda s: GDATE_Y in s)

    fri8 = P.stage("Friday: candles Off, Hebrew date Off")

    @P.part("candles_off", fri8, needs=["friday_before"])
    def _():
        row_set("candle", "0")
        P.expect_absent("Candle lighting row Off: no candles on the clock", lambda s: "/IMG/Candles.bmp" in s)

    @P.part("hebrew_date_off", fri8, needs=["friday_before"])
    def _():
        row_set("hebrewdate", "0")
        P.expect_absent("Hebrew date row Off: the clock stops showing it", lambda s: HDATE in s)

    # Daf Yomi and Mishnah Yomit: Off in the family's settings - switched on they show (checked on
    # jcsim: "דף יומי: מעילה דף כ״ב", the Mishnah icon with "טהרות ג׳:ב׳-ג׳"), back Off they go
    @P.part("daf_on", fri8, needs=["friday_before"])
    def _():
        row_set("dafyomi", "1")
        P.expect("Daf Yomi row on: the day's daf on the clock", lambda s: "דף יומי:" in s)

    @P.part("mishnah_on", fri8, needs=["friday_before"])
    def _():
        row_set("mishnayomit", "1")
        P.expect("Mishnah Yomit row on: the day's mishnayot on the clock", lambda s: "/IMG/Mishnah.bmp" in s)

    fri9 = P.stage("Friday: candles, Hebrew date back")

    @P.part("daf_back", fri9, needs=["daf_on"])
    def _():
        row_set("dafyomi", was["dafyomi"])
        if was["dafyomi"] == "0":
            P.expect_absent("... Daf Yomi back Off: gone", lambda s: "דף יומי:" in s)

    @P.part("mishnah_back", fri9, needs=["mishnah_on"])
    def _():
        row_set("mishnayomit", was["mishnayomit"])
        if was["mishnayomit"] == "0":
            P.expect_absent("... Mishnah Yomit back Off: gone", lambda s: "Mishnah.bmp" in s)

    @P.part("candles_on", fri9, needs=["candles_off"])
    def _():
        row_set("candle", was["candle"])
        P.expect("... candles back: shown again", lambda s: "/IMG/Candles.bmp" in s)

    @P.part("hebrew_date_on", fri9, needs=["hebrew_date_off"])
    def _():
        row_set("hebrewdate", was["hebrewdate"])
        P.expect("... Hebrew date back: shown again", lambda s: HDATE_Y in s)

    # Shabbat Mevarchim before Nisan 5787 = 3 April 2027: "מברכים חודש ניסן" (jcsim) - its row Off, back
    mev = P.stage("2027-04-03 (Shabbat Mevarchim Nisan)", lambda: clock_at("2027-04-03T10:00"))
    MEV = "מברכים חודש ניסן"

    @P.part("mevarchim_before", mev)
    def _():
        P.expect("Shabbat Mevarchim: 'מברכים חודש ניסן' on the clock", lambda s: MEV in s)

    mev2 = P.stage("Shabbat Mevarchim: its row Off")

    @P.part("mevarchim_off", mev2, needs=["mevarchim_before"])
    def _():
        row_set("mevarchim", "0")
        P.expect_absent("Mevarchim row Off: not on the clock", lambda s: MEV in s)

    mev3 = P.stage("Shabbat Mevarchim: its row back")

    @P.part("mevarchim_on", mev3, needs=["mevarchim_off"])
    def _():
        row_set("mevarchim", was["mevarchim"])
        P.expect("... Mevarchim back: shown again", lambda s: MEV in s)

    # The Fast of Esther, 13 Adar II 5787 = Monday 22 March 2027: "צום: 04:30-18:08", "ערב פורים"
    esther = P.stage("2027-03-22 (Fast of Esther)", lambda: clock_at("2027-03-22T10:00"))
    FAST, EREV_PURIM = "צום: ", "ערב פורים"

    @P.part("esther_before", esther)
    def _():
        P.expect("Fast of Esther: its times shown", lambda s: FAST in s)
        P.expect("... and Erev Purim (the holiday row)", lambda s: EREV_PURIM in s)

    esther2 = P.stage("Fast of Esther: fast Off, holiday Off")

    @P.part("fast_off", esther2, needs=["esther_before"])
    def _():
        row_set("fast", "0")
        P.expect_absent("Fast row Off: no fast times on the clock", lambda s: FAST in s)

    @P.part("holiday_off", esther2, needs=["esther_before"])
    def _():
        row_set("holiday", "0")
        P.expect_absent("Holiday row Off: no Erev Purim on the clock", lambda s: EREV_PURIM in s)

    esther3 = P.stage("Fast of Esther: back on")

    @P.part("fast_on", esther3, needs=["fast_off"])
    def _():
        row_set("fast", was["fast"])
        P.expect("... fast back: its times again", lambda s: FAST in s)

    @P.part("holiday_on", esther3, needs=["holiday_off"])
    def _():
        row_set("holiday", was["holiday"])
        P.expect("... holiday back: Erev Purim again", lambda s: EREV_PURIM in s)

    # The Omer's wording (the row's own choice), day 23 = Shabbat 15 May 2027 - each of the four
    omer_sel = '#content-order-list .content-extra[data-cfg="omerMode"]'
    OMER = {"1": ("23 לעומר", "שבועות"), "4": ("23 = 3 שבועות + 2 ימים", "לעומר"),
            "3": ("23 שהם 3 שבועות + 2 ימים לעומר", None), "2": ("היום שלושה ועשרים יום, שהם שלושה שבועות ושני ימים לעומר", None)}
    omer_stage = P.stage("2027-05-15 (Omer day 23)", lambda: clock_at("2027-05-15T10:00"))

    def omer_part(mode, stage, name, needs):
        @P.part(name, stage, needs=needs)
        def _():
            ui.goto(HOME + "/")
            ui.settings()
            was.setdefault("omerMode", pg.input_value(omer_sel))
            pg.select_option(omer_sel, mode)
            check("Omer wording %s: saved" % mode, wait(lambda: cfg().get("omerMode") == int(mode), 6), cfg().get("omerMode"))
            want, never = OMER[mode]
            # Compared without spaces: wording 4 is fit to the window by measuring
            # (MessageAggregator.cpp: "23 = 3 שבועות + 2 ימים לעומר", without "לעומר", or tight
            # "23=3 שבועות+2 ימים") - any of them is right; the test clock's window takes the tight one
            tight = lambda t: t.replace(" ", "")
            P.expect("Omer wording %s: the clock says '%s'" % (mode, want),
                     lambda s: tight(want) in tight(s) and (never is None or mode == "4" or never not in s))

    prev = []
    for i, mode in enumerate(["1", "4", "2", "3"]):
        st = omer_stage if i == 0 else P.stage("Omer day 23: wording %s" % mode)
        omer_part(mode, st, "omer_%s" % mode, prev[-1:])
        prev.append("omer_%s" % mode)

    # The same Shabbat (still 15 May 2027): the Havdalah and Omer rows Off, then back
    shab = P.stage("Omer day 23: Havdalah Off, Omer Off")

    @P.part("havdalah_off", shab, needs=["omer_3"])
    def _():
        row_set("havdalah", "0")
        P.expect_absent("Havdalah row Off: no Havdalah time on the clock", lambda s: "Havdala.bmp" in s)

    @P.part("omer_off", shab, needs=["omer_3"])
    def _():
        row_set("omer", "0")
        P.expect_absent("Omer row Off: no Omer count on the clock", lambda s: "לעומר" in s)

    shab2 = P.stage("Omer day 23: Havdalah, Omer back")

    @P.part("havdalah_on", shab2, needs=["havdalah_off"])
    def _():
        row_set("havdalah", was["havdalah"])
        P.expect("... Havdalah back: shown again", lambda s: "/IMG/Havdala.bmp" in s)

    @P.part("omer_on", shab2, needs=["omer_off"])
    def _():
        row_set("omer", was["omer"])
        P.expect("... Omer back: the count again", lambda s: "לעומר" in s)

    # The rain/dew row's two choices, on 16 Nisan 5787 = 23 April 2027 (the day after the switch,
    # checked on jcsim 2026-11-15): days 0 -> no slide; 1 or 2 -> "מוריד הטל, ותן ברכה" (Ashkenaz) /
    # "מוריד הטל, ברכנו" (Sefard)
    psel_x = lambda k: '#content-order-list .content-extra[data-cfg="%s"]' % k
    DEW, ASH, SEF = "מוריד הטל", "מוריד הטל, ותן ברכה", "מוריד הטל, ברכנו"

    def extra_set(key, v):
        ui.goto(HOME + "/")
        ui.settings()
        was.setdefault(key, pg.input_value(psel_x(key)))
        pg.select_option(psel_x(key), v)
        return check("%s set to %s: saved" % (key, v), wait(lambda: cfg().get(key) == int(v), 6), cfg().get(key))

    dew0 = P.stage("2027-04-23 (day after the dew switch): days 0", lambda: clock_at("2027-04-23T10:00"))

    @P.part("dew_days_0", dew0)
    def _():
        extra_set("prayerSwitchDays", "0")
        P.expect_absent("Rain/dew days 0 ('only on the day'): nothing the day after", lambda s: DEW in s)

    dew2 = P.stage("day after the dew switch: days 2, Sefard")

    @P.part("dew_days_2_sefard", dew2, needs=["dew_days_0"])
    def _():
        extra_set("prayerSwitchDays", "2")
        extra_set("nusach", "1")
        P.expect("... days 2, Sefard: 'מוריד הטל, ברכנו' the day after", lambda s: SEF in s)

    dew3 = P.stage("day after the dew switch: Ashkenaz")

    @P.part("dew_ashkenaz", dew3, needs=["dew_days_2_sefard"])
    def _():
        extra_set("nusach", "0")
        P.expect("... Ashkenaz: 'מוריד הטל, ותן ברכה'", lambda s: ASH in s)

    dew3b = P.stage("day after the dew switch: its row Off")

    @P.part("dew_row_off", dew3b, needs=["dew_ashkenaz"])
    def _():
        row_set("prayerswitch", "0")
        P.expect_absent("Rain/dew row Off: no wording on the clock", lambda s: DEW in s)

    dew4 = P.stage("day after the dew switch: as before")

    @P.part("dew_back", dew4, needs=["dew_row_off"])
    def _():
        row_set("prayerswitch", was["prayerswitch"])
        extra_set("prayerSwitchDays", was["prayerSwitchDays"])
        extra_set("nusach", was["nusach"])

    # "When an event on a Hebrew date starts" (the events row's choice): an event on 20 Nisan (no
    # year) = Tuesday 27 April 2027; at 21:30 the evening before, after nightfall - shown when it
    # starts the evening before, not when it starts at midnight
    evd = P.stage("2027-04-26 21:30 (evening before 20 Nisan)", lambda: clock_at("2027-04-26T21:30"))
    EVD = "UIT DayChange"

    @P.part("event_evening", evd)
    def _():
        code, b = api_post("/api/events/upsert", {"name": EVD, "hasG": False, "gD": 0, "gM": 0, "gY": 0, "hasJ": True, "jD": 20, "jMonth": "ניסן",
                                                   "jYNum": 0, "type": "other", "icon": "", "warn": 0, "image": ""})
        check("an event on 20 Nisan (no year): saved", code == 200 and find_event(EVD), (code, b[:120]))
        extra_set("eventDayChange", "0")
        P.expect("Hebrew-date events from the evening before: shown at 21:30 the evening before", lambda s: EVD in s)

    evd2 = P.stage("evening before 20 Nisan: events from midnight")

    @P.part("event_midnight", evd2, needs=["event_evening"])
    def _():
        extra_set("eventDayChange", "1")
        P.expect_absent("... from midnight: not yet at 21:30", lambda s: EVD in s)

    evd3 = P.stage("evening before 20 Nisan: as before")

    @P.part("event_daychange_back", evd3, needs=["event_midnight"])
    def _():
        extra_set("eventDayChange", was["eventDayChange"])
        remove_test_events("UIT DayChange")

    # When the timed slides show (their rows' choices), checked on jcsim 2026-11-15:
    # Thursday 11 March 2027 21:00 - candles only "From evening before", as "מחר 17:26" with
    # "Tomorrow" ticked, "17:26" without; Monday 8 March 10:00 - the parasha with "All week",
    # not "From Thu evening"
    tick = lambda k: '#content-order-list .content-tick[data-cfg="%s"]' % k
    CAND = "/IMG/Candles.bmp"
    thu = P.stage("2027-03-11 21:00 (Thursday evening): candles from the evening before", lambda: clock_at("2027-03-11T21:00"))

    @P.part("candles_from_evening", thu)
    def _():
        extra_set("candleFrom", "1")
        ui.goto(HOME + "/")
        ui.settings()
        was.setdefault("candleTomorrow", pg.is_checked(tick("candleTomorrow")))
        if not pg.is_checked(tick("candleTomorrow")):
            pg.check(tick("candleTomorrow"))
        check("... 'Tomorrow' ticked: saved", wait(lambda: cfg().get("candleTomorrow") is True, 6))
        P.expect("Candles from the evening before: shown Thursday evening, 'מחר' before the time", lambda s: CAND in s and "מחר" in s)

    thu2 = P.stage("Thursday evening: 'Tomorrow' off")

    @P.part("candles_no_tomorrow", thu2, needs=["candles_from_evening"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.uncheck(tick("candleTomorrow"))
        check("'Tomorrow' off: saved", wait(lambda: cfg().get("candleTomorrow") is False, 6))
        P.expect("... the time without 'מחר'", lambda s: CAND in s and "מחר" not in s)

    thu3 = P.stage("Thursday evening: candles from midnight")

    @P.part("candles_from_midnight", thu3, needs=["candles_no_tomorrow"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        if pg.is_checked(tick("candleTomorrow")) != was["candleTomorrow"]:
            pg.click(tick("candleTomorrow"))
        wait(lambda: cfg().get("candleTomorrow") == was["candleTomorrow"], 6)
        extra_set("candleFrom", "0")
        P.expect_absent("... from midnight: no candles on Thursday evening", lambda s: CAND in s)

    mon = P.stage("2027-03-08 10:00 (Monday): parasha around Shabbat only", lambda: clock_at("2027-03-08T10:00"))

    @P.part("parasha_around_shabbat", mon)
    def _():
        extra_set("parashaFrom", "1")
        P.expect_absent("Parasha 'From Thu evening': not on a Monday", lambda s: "Torah.bmp" in s)

    mon2 = P.stage("Monday: parasha all week")

    @P.part("parasha_all_week", mon2, needs=["parasha_around_shabbat"])
    def _():
        extra_set("parashaFrom", "2")
        P.expect("... 'All week': on Monday too", lambda s: "/IMG/Torah.bmp" in s)

    mon3 = P.stage("Monday: when the timed slides show, as before")

    @P.part("show_windows_back", mon3, needs=["parasha_all_week", "candles_from_midnight"])
    def _():
        extra_set("candleFrom", was["candleFrom"])
        extra_set("parashaFrom", was["parashaFrom"])

    # "When it stops showing" (candles / Havdalah / fast), checked on jcsim 2026-11-15: just after
    # the moment, "Until <it>" -> gone, "Until midnight" -> still shown; then as before
    def until_chain(key, at, label, marker):
        st0 = P.stage("%s: %s 'Until %s'" % (at, key, label), lambda: clock_at(at))

        @P.part(key + "_until_it", st0)
        def _():
            extra_set(key, "0")
            P.expect_absent("%s 'Until %s': gone just after it (%s)" % (key, label, at), lambda s: marker in s)

        st1 = P.stage("%s: %s 'Until midnight'" % (at, key))

        @P.part(key + "_until_midnight", st1, needs=[key + "_until_it"])
        def _():
            extra_set(key, "1")
            P.expect("... 'Until midnight': still shown then", lambda s: marker in s)

        st2 = P.stage("%s: %s as before" % (at, key))

        @P.part(key + "_until_back", st2, needs=[key + "_until_midnight"])
        def _():
            extra_set(key, was[key])

    until_chain("candleUntil", "2027-03-12T18:30", "lighting", "/IMG/Candles.bmp")
    until_chain("havdalahUntil", "2027-03-13T20:00", "Havdalah", "/IMG/Havdala.bmp")
    until_chain("fastUntil", "2027-03-22T19:00", "fast end", "צום: ")

    omer_back = P.stage("Omer day 23: wording as before")

    @P.part("omer_back", omer_back, needs=["omer_3"])
    def _():
        ui.goto(HOME + "/")
        ui.settings()
        pg.select_option(omer_sel, was["omerMode"])
        check("... Omer wording back as it was: saved", wait(lambda: cfg().get("omerMode") == int(was["omerMode"]), 6))

    # ── Stage: 15 March 2027 - the event's day; deleted / restored ──
    mar15 = P.stage("2027-03-15", lambda: clock_at("2027-03-15T10:00"))

    @P.part("event_day", mar15, needs=["full_event"])
    def _():
        P.expect("... on the day: with its age (27), cake icon", lambda s: "UIT Rem" in s and "(27)" in s and "/IMG/Cake.bmp" in s)

    mar15x = P.stage("2027-03-15: the events row Off")

    @P.part("events_off", mar15x, needs=["event_day"])
    def _():
        row_set("events", "0")
        P.expect_absent("Events row Off: the clock shows no events, not even on the day", lambda s: "UIT Rem" in s)

    mar15y = P.stage("2027-03-15: the events row back")

    @P.part("events_on", mar15y, needs=["events_off"])
    def _():
        row_set("events", was["events"])
        P.expect("... events back: the birthday again", lambda s: "UIT Rem" in s)

    mar15b = P.stage("2027-03-15: deleted")

    @P.part("deleted", mar15b, needs=["full_event"])
    def _():
        ui.goto(HOME + "/")
        pg.evaluate("editEvent(events.findIndex(e => e.name === 'UIT Rem'))")
        pg.wait_for_timeout(800)
        pg.click("#btn-del")
        pg.wait_for_timeout(500)
        if pg.locator("#cf-buttons button").count():
            pg.locator("#cf-buttons button").first.click()
        check("deleted: gone from the events", wait(lambda: find_event("UIT Rem") is None, 10, 0.3))
        P.expect_absent("deleted: the clock no longer shows it on its day", lambda s: "UIT Rem" in s)

    mar15c = P.stage("2027-03-15: restored")

    @P.part("restored", mar15c, needs=["deleted"])
    def _():
        binned = [e for e in jget("/api/events/deleted") if e["name"] == "UIT Rem"]
        http(HOME, "/api/events/restore", form={"id": binned[0]["id"] if binned else "x"})
        P.expect("restored from the bin: the clock shows it again", lambda s: "UIT Rem" in s)

    # ── Stage: its Hebrew day in 2027 ──
    def to_hebrew_day():
        hd = convert(**{"from": "g", "y": 2000, "m": 3, "d": 15})
        g27 = convert(**{"from": "h", "y": 5787, "month": hd["month"], "d": hd["d"], "isAdarII": hd.get("isAdarII", False)})
        got["g27"] = g27
        clock_at("%04d-%02d-%02dT10:00" % (g27["y"], g27["m"], g27["d"]))
    hday = P.stage("its Hebrew day, 2027", to_hebrew_day)

    @P.part("hebrew_day", hday, needs=["full_event"])
    def _():
        g27 = got["g27"]
        P.expect("... on its Hebrew day (%d/%d/%d): with the Hebrew-date mark" % (g27["d"], g27["m"], g27["y"]),
                 lambda s: "UIT Rem" in s and "/IMG/HebrewDate.bmp" in s)

    # ── The page only: the Hebrew boxes' tooltips ──
    page_only = P.stage("page only", lambda: clock_at(None))

    @P.part("tooltips", page_only)
    def _():
        ui.goto(HOME + "/")
        pg.click("button.fab")
        pg.wait_for_timeout(1000)
        pg.check("#ev-has-g")
        pg.fill("#ev-g-d", "22"); pg.select_option("#ev-g-m", "11"); pg.fill("#ev-g-y", "2033")
        hd = convert(**{"from": "g", "y": 2033, "m": 11, "d": 22})
        # (the Hebrew side comes from the clock's conversion: a fixed 1.2 s read the old one under load)
        wait(lambda: (" = %d" % hd["d"]) in (pg.get_attribute("#ev-j-d", "title") or "")
             and ("= %d (" % hd["y"]) in (pg.get_attribute("#ev-j-y", "title") or ""), 10)   # both boxes: the year came later once
        pg.hover("#ev-j-d"); dt = pg.get_attribute("#ev-j-d", "title") or ""
        pg.hover("#ev-j-y"); yt = pg.get_attribute("#ev-j-y", "title") or ""
        check("Hebrew day tooltip: today's value in the box, as a number", (" = %d" % hd["d"]) in dt, dt)
        check("Hebrew year tooltip: the year in the box, with its Gregorian years", ("= %d (%d/%d)" % (hd["y"], hd["y"] - 3761, hd["y"] - 3760)) in yt, yt)
        pg.fill("#ev-g-y", "2040")
        h40 = convert(**{"from": "g", "y": 2040, "m": 11, "d": 22})
        pg.hover("#ev-g-d")
        wait(lambda: ("= %d" % h40["y"]) in (pg.get_attribute("#ev-j-y", "title") or ""), 10)   # was a fixed 1.2 s
        pg.hover("#ev-j-y"); yt2 = pg.get_attribute("#ev-j-y", "title") or ""
        check("... after the date changed, the tooltip follows", ("= %d" % h40["y"]) in yt2 and yt2 != yt, yt2)
        pg.keyboard.press("Escape")

    try:
        P.run()
    finally:
        clock_at(None)
        api_post("/api/builtin/reset?name=Torah", {})
        remove_test_events()
    check("chains: test events removed", not [e for e in events() if e["name"].startswith("UIT")])


def heb_year(y):
    """A Hebrew year as the clock writes it, without the thousands: 5787 -> תשפ״ז."""
    n, out = y % 1000, ""
    for v, ch in ((400, "ת"), (300, "ש"), (200, "ר"), (100, "ק")):
        while n >= v:
            out += ch
            n -= v
    if n in (15, 16):
        out += "ט" + "וז"[n - 15]
        n = 0
    for v, ch in ((90, "צ"), (80, "פ"), (70, "ע"), (60, "ס"), (50, "נ"), (40, "מ"), (30, "ל"), (20, "כ"), (10, "י"),
                  (9, "ט"), (8, "ח"), (7, "ז"), (6, "ו"), (5, "ה"), (4, "ד"), (3, "ג"), (2, "ב"), (1, "א")):
        if n >= v:
            out += ch
            n -= v
    return out[:-1] + "״" + out[-1] if len(out) > 1 else out + "׳"


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
    check("Reset Wi-Fi asks first (No: nothing - watched 3 s)", stays(lambda: cfg()["wifiSSID"] == c0["wifiSSID"] and not in_setup_mode(), 3, 0.5))
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
    check("Connect: saved", wait(lambda: W().has("setupSaved", pg.inner_text("body")), 10), pg.inner_text("body")[:100])
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
    check("... the link (or the page by itself, its refresh) opens the clock's page", pg.url.rstrip("/") == HOME and wait(lambda: pg.is_visible("#view-calendar"), 10), pg.url)
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
    check("Restore from backup: its page", wait(lambda: pg.is_visible("#restore-view") and not pg.is_visible("#view-calendar"), 10))
    with pg.expect_file_chooser() as fc:
        pg.click("button:has-text('Choose backup file')")
    fc.value.set_files(f("notapic.txt"))
    check("not a backup: says so", wait(lambda: "not a backup from this clock" in pg.inner_text("#restore-msg"), 10), pg.inner_text("#restore-msg"))
    newer = "%d.0.0" % (int(xlsx_made_by(backup).split(".")[0]) + 1)
    with pg.expect_file_chooser() as fc:
        pg.click("button:has-text('Choose backup file')")
    fc.value.set_files(xlsx_restamp(backup, OUT / "files" / "backup_newer.xlsx", newer))
    said = wait(lambda: (lambda t: t if W().has("verNewer", t) else None)(pg.inner_text("#restore-msg")), 10, 0.3)
    check("a backup made by a newer version (%s): refused, nothing changed, still factory-new" % newer,
          said and PW().has("nothingChanged", said) and in_setup_mode(), pg.inner_text("#restore-msg"))
    with pg.expect_file_chooser() as fc:
        pg.click("button:has-text('Choose backup file')")
    fc.value.set_files(str(backup))
    check("the backup: restored", wait(lambda: PW().has("restoredAll", pg.inner_text("#restore-msg")), 60, 1), pg.inner_text("#restore-msg"))
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

@section
def from_family(ui, clock):
    """The family's path (was utils/ap_tests.py --sim, 2026-11-11): a clock at their version with their
    data, on its own Wi-Fi - the page, the update to this build (everything kept), a restore of the old
    version's backup on a factory-new clock."""
    import gzip
    import io
    import tarfile
    P = Parts("from_family")
    home, ap, ctl_url = urls("from_family", PORT_OFF[0])
    d = OUT / "family_clock"
    st = {}
    lf = lambda b: b.replace(b"\r\n", b"\n")

    def get(base, path, timeout=30):
        """As the browser sees it: the page files come gzipped (utils/web_files.py)."""
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            data = r.read()
            return gzip.decompress(data) if r.headers.get("Content-Encoding") == "gzip" else data

    def fam_status():
        try:
            return json.loads(get(home, "/api/status", 5))
        except Exception:
            return {}

    def setup_mode():
        try:
            r = urllib.request.build_opener(_NoRedirect).open(ap + "/wifi-scan", timeout=4)
            b = r.read()
            return r.status == 200 and b.lstrip().startswith(b"{") and b"networks" in b
        except Exception:
            return False

    def snapshot():
        backup = json.loads(get(home, "/api/backup", 60))
        images = sorted(x["id"] for x in json.loads(get(home, "/api/images/list", 60)))
        try:   # pictures of deleted events stay in the recycle bin (since 2.1.2)
            binned = {e["image"] for e in json.loads(get(home, "/api/events/deleted", 30)) if e.get("image")}
        except Exception:
            binned = set()
        return {"config": backup["config"], "events": backup["events"], "images": sorted(set(images) - binned)}

    def unchanged(label):
        before, after = st["before"], snapshot()
        def filled_ntp(old, new):   # an old one-server list gets the defaults after it (Storage::readNtpServers(),
            # 2026-11-13) - written to the file once the clock saves its settings (since 2026-11-14 at once:
            # its own Wi-Fi password goes into them)
            return isinstance(old, list) and isinstance(new, list) and len(old) < len(new) == lim_ntp and new[:len(old)] == old
        lim_ntp = json.loads(get(home, "/api/limits")).get("ntpServerSlots", 3) if before["config"].get("ntpServers") else 0
        def same_places(old, new):   # a newer version may add a field to each place (countryCode, 2026-11-21): the old ones stay
            return (isinstance(old, list) and isinstance(new, list) and len(old) == len(new)
                    and all(isinstance(o, dict) and isinstance(n, dict) and all(n.get(f) == v for f, v in o.items())
                            for o, n in zip(old, new)))
        diffs = ["config.%s: %r -> %r" % (k, before["config"][k], after["config"].get(k)) for k in sorted(before["config"])
                 if k not in ("nightPct", "roomLightPct")   # learned by the clock itself (BrightnessManager.cpp)
                 and before["config"][k] != after["config"].get(k)
                 and not (k == "ntpServers" and filled_ntp(before["config"][k], after["config"].get(k)))
                 and not (k == "places" and same_places(before["config"][k], after["config"].get(k)))]
        def norm(e):   # without "id": an Excel import gives every event a new one
            e = {k: v for k, v in e.items() if k != "id"}
            if not e.get("jYNum") and e.get("jY") == "?":   # a yearless Hebrew date's year text: "?" before 2026-11-18, "" since (28176bc)
                e["jY"] = ""
            return e
        ev = lambda s: sorted(json.dumps(norm(e), sort_keys=True, ensure_ascii=False) for e in s["events"])
        if ev(before) != ev(after):   # without "id": an Excel import gives every event a new one
            diffs.append("events: %d -> %d, %d differ" % (len(before["events"]), len(after["events"]), len(set(ev(before)) ^ set(ev(after)))))
        if before["images"] != after["images"]:
            diffs.append("pictures: %s -> %s" % (before["images"], after["images"]))
        check(label, not diffs, "; ".join(diffs)[:3000] or "settings, events and pictures as at the start")

    def pages_are(data_dir):
        wait(lambda: fam_status().get("synced"), 90, 1)   # before the time sync "/" is the "initializing" page
        return [p for p in ("index.html", "update.html", "sortable.min.js")
                if lf(get(home, "/" if p == "index.html" else "/" + p)) != lf((data_dir / p).read_bytes())]

    def start():
        """Their version's build (sim/build_clock.py keeps each release's), its page files from its tag,
        their settings, events and pictures (Data/)."""
        exe = Path("C:/pio3/jc_clean/sim/versions") / FROM[0] / "jcclock.exe"
        if not check("the family's version %s has a simulator build" % FROM[0], exe.exists(), exe):
            raise RuntimeError("no build of %s" % FROM[0])
        rmtree(d)
        tar = subprocess.run(["git", "archive", "v" + FROM[0], "Data"], cwd=ROOT, capture_output=True, check=True).stdout
        tarfile.open(fileobj=io.BytesIO(tar)).extractall(d / "from")
        st["pages_from"] = d / "from" / "Data"
        web_files.stage(st["pages_from"], d / "fs")
        for name in ("config.json", "events.json"):
            shutil.copy2(ROOT / "Data" / name, d / "fs" / name)
        # The one change to their data: Cloudflare first among the time servers. Their version asks only
        # the first, pool.ntp.org - one member of which was silent for minutes three times on 2026-11-20/21
        # (185.18.207.158), and this section failed at "starts and syncs" on it. Not what it tests.
        cf = d / "fs" / "config.json"
        c_ = json.loads(cf.read_text(encoding="utf-8"))
        c_["ntpServers"] = ["time.cloudflare.com"] + [s for s in c_.get("ntpServers", []) if s != "time.cloudflare.com"][:2]
        cf.write_text(json.dumps(c_, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        for sub in ("images", "custom"):   # not in git, so not in the tag
            if (ROOT / "Data" / sub).is_dir():
                shutil.copytree(ROOT / "Data" / sub, d / "fs" / sub, dirs_exist_ok=True)
        (d / "state").mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, d / "jcclock.exe")
        ports_ = ports("from_family", PORT_OFF[0])
        st["log"] = open(d / "run.log", "ab")
        st["proc"] = subprocess.Popen([str(d / "jcclock.exe"), "--fs", str(d / "fs"), "--state", str(d / "state"), "--verbose",
                                       "--home-port", str(ports_[0]), "--ap-port", str(ports_[1]), "--control-port", str(ports_[2])],
                                      stdout=st["log"], stderr=subprocess.STDOUT, cwd=str(d), env=dict(os.environ, JC_TITLE="Test (from_family)"))
        check("their version starts and syncs", wait(lambda: fam_status().get("synced"), 120, 1), fam_status().get("firmware"))
        st["before"] = snapshot()
        # Its own browser window, outside the coverage count: the old version's page is not this one
        st["ctx"] = ui.browser.new_context(viewport={"width": 1280, "height": 900}, accept_downloads=True)
        st["pg"] = st["ctx"].new_page()
        st["pg"].set_default_timeout(60000)
        st["errors"] = []
        st["pg"].on("pageerror", lambda e: st["errors"].append(str(e)))
        st["pg"].on("dialog", lambda dl: dl.accept())

    P.stage("their version, their data", start)

    @P.part("page_over_ap", "their version, their data")
    def _():
        pg = st["pg"]
        pg.goto(ap + "/", wait_until="load")
        pg.wait_for_function("events.length > 0 && el('cfg-name').value !== ''", timeout=60000)
        check("over the clock's own Wi-Fi: the page with their events", pg.evaluate("events.length") == len(st["before"]["events"]),
              (pg.evaluate("events.length"), len(st["before"]["events"])))
        bad = pages_are(st["pages_from"])
        check("... its page files are their version's", not bad, bad)

    @P.part("export", "their version, their data", needs=("page_over_ap",))
    def _():
        pg = st["pg"]
        pg.click("button.rail-item[data-view=settings]")
        pg.click("#scope-both")
        with pg.expect_download(timeout=120000) as dl:
            pg.click("button:has-text('Export')")
        st["xlsx"] = OUT / "files" / ("family_" + dl.value.suggested_filename)
        st["xlsx"].parent.mkdir(parents=True, exist_ok=True)
        dl.value.save_as(str(st["xlsx"]))
        check("their version's Excel export (Both)", st["xlsx"].stat().st_size > 5000, st["xlsx"].stat().st_size)

    @P.part("update", "their version, their data", needs=("page_over_ap",))
    def _():
        pg = st["pg"]
        if PACKAGE[0]:
            pkg = Path(PACKAGE[0])
            pv = pkg.name[len("JewishClock-update-"):-len(".jcupdate")]
        else:
            pkg, pv = make_package()
        if not check("this build's update package (and its simulator build)", pkg and (Path("C:/pio3/jc_clean/sim/versions") / pv / "jcclock.exe").exists(), (pkg, pv)):
            return
        pg.goto(ap + "/update.html", wait_until="load")
        pg.wait_for_function("el('curVer').textContent !== 'checking...'", timeout=30000)
        check("their update page shows their version", pg.inner_text("#curVer") == FROM[0], pg.inner_text("#curVer"))
        pg.set_input_files("#fileInput", str(pkg))
        pg.click("#goBtn")
        def done():
            try:
                cls, msg = pg.evaluate("[el('msg').className, el('msg').textContent]")
            except Exception:
                return None   # the page in the middle of a reload
            return (cls, msg) if cls in ("ok", "err") and "Sending" not in msg else None
        end = wait(done, 600, 2)
        check("installed over the clock's Wi-Fi by their version's code: Done", end and end[0] == "ok" and end[1].startswith("Done!"), end)
        s = fam_status()
        check("... the new version runs, its trial passed", s.get("firmware") == pv and s.get("updateState") == "ok", (s.get("firmware"), s.get("updateState")))
        unchanged("... their settings, events and pictures kept")
        bad = pages_are(ROOT / "Data")   # in only after the trial passed
        check("... the page files are this version's", not bad, bad)

    @P.part("restore_old_backup", "their version, their data", needs=("export", "update"))
    def _():
        pg = st["pg"]
        http(home, "/api/factory-reset", form={"body": "{}"}, raw=True)
        check("factory reset: the setup Wi-Fi", wait(setup_mode, 120, 2))
        pg.goto(ap + "/restore", wait_until="load")
        pg.set_input_files("#restore-file", str(st["xlsx"]))
        msg = wait(lambda: (lambda t: t if re.search(r"^Restored:|not restored|not a backup|no usable|Damaged", t) else None)(pg.inner_text("#restore-msg")), 300, 2)
        check("their old version's backup restored on the new one", bool(msg) and PW(pg).has("restoredAll", msg), msg)
        # Over the clock's own Wi-Fi the page says to choose that Wi-Fi again - once the clock stops
        # answering (it restarted), not together with "Restored" (2026-11-09, /api/status viaAp)
        from ap_trip import CLOCK_AP
        def text():
            try:
                return pg.evaluate("(document.getElementById('restore-msg') || {}).textContent || ''")
            except Exception:
                return ""   # the page on its way to "/"
        said = wait(lambda: (lambda t: t if 'Choose the Wi-Fi "%s" again' % CLOCK_AP in t else None)(text()), 120, 0.3)
        check("... the page names the clock's Wi-Fi once it restarts", said and "Choose the Wi-Fi" not in (msg or ""), (said or "not said")[-120:])
        check("... the clock back on the home Wi-Fi", wait(lambda: fam_status().get("synced"), 240, 2))
        unchanged("... everything as before the update")

    @P.part("no_page_errors", "their version, their data")
    def _():
        check("no script errors on their pages", not st["errors"], st["errors"][:5])

    try:
        P.run()
    finally:
        if st.get("ctx"):
            st["ctx"].close()
        if st.get("proc"):
            try:
                http(ctl_url, "/stop", data=b"", timeout=5)
            except Exception:
                pass   # it closes the connection as it stops
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(st["proc"].pid)], capture_output=True)
            st["log"].close()


@section
def refusals(ui, clock):
    """What the clock refuses from any client - not only from the page, which stops most of it first
    (2026-11-18, from the coverage list: these answers had never been asked for). Each: the answer,
    its words, and nothing changed on the clock. Then the requests it takes with a fix of its own:
    a missing date side filled in, "after sunset" set where only that matches, a Hebrew date
    without a year shown without one, debug time by timestamp and through the settings."""
    def kept():   # the clock's settings and events, to compare
        s = clock_snapshot()
        if "config" in s:
            s["config"] = {k: v for k, v in s["config"].items() if k not in SNAPSHOT_SELF}
        return s
    before = kept()

    def refused(what, code_said, code, words):
        c, b = code_said
        said = b.decode("utf-8", "replace") if isinstance(b, bytes) else str(b)
        check("refused: %s - %d, says why" % (what, code), c == code and all(W().has(w[0], said, *w[1:]) if isinstance(w, tuple) else w in said for w in words), (c, said[:200]))
        check("... nothing changed", kept() == before)

    def ev(**kw):
        e = {"name": "UIT Ref", "hasG": False, "gD": 0, "gM": 0, "gY": 0, "hasJ": False, "jD": 0, "jMonth": "", "jYNum": 0,
             "isAdarII": False, "afterSunset": False, "type": "other", "icon": "", "warn": 0, "image": ""}
        e.update(kw)
        return e

    def upload_icon(name, data):
        boundary = "uitboundary7"
        body = ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x.bmp\"\r\nContent-Type: image/bmp\r\n\r\n" % boundary).encode()
        body += data + ("\r\n--%s--\r\n" % boundary).encode()
        req = urllib.request.Request(HOME + "/api/builtin/upload?name=" + urllib.parse.quote(name), data=body)
        req.add_header("Content-Type", "multipart/form-data; boundary=" + boundary)
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    # ── events ──
    refused("an event that isn't JSON", http(HOME, "/api/events/upsert", form={"body": "{not json"}, raw=True), 400, [("invalidJson",)])
    refused("an event without a name (spaces only)", api_post("/api/events/upsert", ev(name="   ", hasG=True, gD=1, gM=1)), 400,
            [("needName",)])
    # 14 Mar 1990 is 17 Adar 5750: given with only the Gregorian year, the Hebrew day must be 17 Adar
    refused("Gregorian with a year, Hebrew without, not the same day", api_post("/api/events/upsert",
            ev(hasG=True, gD=14, gM=3, gY=1990, hasJ=True, jD=20, jMonth="אדר")), 400, [("dateMismatchG", "14/3/1990")])
    refused("Hebrew with a year, Gregorian without, not the same day", api_post("/api/events/upsert",
            ev(hasG=True, gD=1, gM=1, hasJ=True, jD=17, jMonth="אדר", jYNum=5750)), 400, [("dateMismatchH", None, None, "1/1")])
    refused("a Hebrew date without its month", api_post("/api/events/upsert", ev(hasJ=True, jD=5, jMonth="")), 400, [("hMonthRange", 0)])
    refused("an import check that isn't a list", http(HOME, "/api/events/check", form={"body": "{}"}, raw=True), 400, [("invalidJsonArray",)])
    refused("an import with a nameless row", api_post("/api/events/import", events() + [ev(name="", hasG=True, gD=2, gM=2)]), 400, [("importNoName",)])
    refused("deleting an event that isn't there", http(HOME, "/api/events/delete", form={"id": "uit-no-such-id"}, raw=True), 404, [("eventNotFound",)])
    refused("restoring an event that isn't in the bin", http(HOME, "/api/events/restore", form={"id": "uit-no-such-id"}, raw=True), 404,
            [("notInBin",)])
    # ── other requests ──
    refused("'seen' for a problem that doesn't exist", api_post("/api/problems/seen", {"which": "uit"}), 400, [("unknownProblem",)])
    refused("debug time on 31 February", api_post("/api/debug/time", {"local": "2026-02-31T10:00"}), 400, [("notDateTime", "2026-02-31T10:00")])
    # a year far past any int: the clock's date parsers read with field widths (2026-11-21: "%d" overflow
    # was undefined) - refused, never wrapped into some other year
    refused("debug time in the year 99999999999", api_post("/api/debug/time", {"local": "99999999999-01-01T10:00"}), 400,
            [("notDateTime", "99999999999-01-01T10:00")])
    refused("debug time in 1800",api_post("/api/debug/time", {"local": "1800-01-01T10:00"}), 400, [("debugYears",)])
    refused("debug time without a time", api_post("/api/debug/time", {}), 400, [("debugTimeExpected",)])
    icons = jget("/api/builtin/list")["pictures"]
    lim = jget("/api/limits")["iconsMaxBytes"]
    refused("a replaced icon over the icons' room (%d KB)" % (lim // 1024), upload_icon(icons[0]["name"], b"\0" * (lim + 1024)), 400, [("iconsTooBig", lim // 1024)])
    refused("an icon the clock doesn't have", upload_icon("uit-no-such-icon", b"BM" + b"\0" * 64), 400, [("noSuchPicture",)])

    # ── taken, with the clock's own fix ──
    c, b = api_post("/api/events/upsert", ev(name="UIT Ref G", hasG=True, hasJ=True, jD=17, jMonth="אדר", jYNum=5750))
    e = find_event("UIT Ref G") or {}
    check("both dates on, the Gregorian empty: filled from the Hebrew (17 Adar 5750 -> 14/3/1990)",
          c == 200 and (e.get("gD"), e.get("gM"), e.get("gY")) == (14, 3, 1990), (c, b[:120], e))
    c, b = api_post("/api/events/upsert", ev(name="UIT Ref H", hasG=True, gD=14, gM=3, gY=1990, hasJ=True))
    e = find_event("UIT Ref H") or {}
    check("both dates on, the Hebrew empty: filled from the Gregorian (14/3/1990 -> 17 Adar 5750)",
          c == 200 and (e.get("jD"), e.get("jMonth"), e.get("jYNum")) == (17, "אדר", 5750), (c, b[:120], e))
    c, b = api_post("/api/events/upsert", ev(name="UIT Ref S", hasG=True, gD=13, gM=3, gY=1990, hasJ=True, jD=17, jMonth="אדר"))
    e = find_event("UIT Ref S") or {}
    check("13/3/1990 with 17 Adar: they meet only after sunset - saved so", c == 200 and e.get("afterSunset") is True, (c, b[:120], e))
    e = find_event("UIT Ref S") or {}
    check("... a Hebrew date without a year has no year text (was \"?\": '17 אדר ?' in the search)", e.get("jY") == "", e.get("jY"))
    remove_test_events("UIT Ref")
    real = status()["utcTime"]
    c, b = api_post("/api/debug/time", {"timestamp": real + 86400})
    check("debug time by a timestamp (scripts): the clock a day on", c == 200 and wait(lambda: abs(status()["utcTime"] - real - 86400) < 30, 10, 0.5),
          (c, b[:120]))
    c, b = api_post("/api/config", {"debugTimeOffset": 3600})
    check("debug time through the settings (sim/moments.py's way): an hour on, at once",
          c == 200 and cfg()["debugTimeOffset"] == 3600 and wait(lambda: abs(status()["utcTime"] - time.time() - 3600) < 30, 10, 0.5), (c, b[:120]))
    api_post("/api/debug/time", {"offset": 0})
    check("... back to the real time", wait(lambda: cfg()["debugTimeOffset"] == 0 and abs(status()["utcTime"] - time.time()) < 30, 10, 0.5))
    check("the clock as it was at the start", wait(lambda: kept() == before, 10, 1))


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


LANE_PORTS = 1000   # --jobs: lane k's clock on the ui ports + k * this (9060.., clear of family 8070 / dev 8080)


def run_side_by_side(only, jobs, base_off, args):
    """The sections shared out over `jobs` lanes - each its own process, clock, browser and
    ports - by the last run's section times (longest first, to the lane with least so far);
    each lane keeps the sections' usual order (setup_and_restore, with its factory reset,
    stays last in its lane). Coverage: every lane's seen/used merged, judged once (2026-11-11)."""
    import re
    took = {}
    for d in sorted(OUT.parent.glob("*/sections.json"), reverse=True):
        try:
            took = json.loads(d.read_text(encoding="utf-8"))
            break
        except ValueError:
            pass
    # Which sections share a lane and their order in it: at random each run (its seed, 2026-11-20) -
    # every section starts on the clock as it was (put_back), so any neighbour must do; still
    # balanced (longest first, +-40 %, to the lane with least so far)
    rnd = test_random("web-ui lanes")
    lanes = [[] for _ in range(jobs)]
    load = [0] * jobs
    for name in sorted(only, key=lambda n: -took.get(n, 60) * rnd.uniform(0.6, 1.4)):
        k = load.index(min(load))
        lanes[k].append(name)
        load[k] += took.get(name, 60)
    procs = []
    for k, lane in enumerate(l for l in lanes if l):
        rnd.shuffle(lane)
        lane.sort(key=lambda n: n in LAST_IN_LANE)   # stable: the factory reset still last
        d = OUT / ("lane%d" % k)
        d.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(Path(__file__).resolve()), "--only", ",".join(lane), "--port-offset", str(base_off + LANE_PORTS * k),
               "--out", str(d), "--lane", "--from", FROM[0]] + (["--package", PACKAGE[0]] if PACKAGE[0] else []) \
            + [a for a in args if a in ("--show",)] \
            + (["--js-coverage", args[args.index("--js-coverage") + 1]] if "--js-coverage" in args else [])
        log("lane %d (~%d s last time): %s" % (k, sum(took.get(n, 60) for n in lane), ", ".join(lane)))
        procs.append((k, d, subprocess.Popen(cmd, stdout=open(d / "out.txt", "wb"), stderr=subprocess.STDOUT)))
    sections = {}
    for k, d, p in procs:
        p.wait()
        text = (d / "summary.txt").read_text(encoding="utf-8") if (d / "summary.txt").exists() else ""
        with open(OUT / "summary.txt", "a", encoding="utf-8") as f:
            f.write("== lane %d ==\n%s" % (k, text))
        print(text, end="", flush=True)
        for line in text.splitlines():
            m = re.match(r"\d\d:\d\d:\d\d (PASS|FAIL) \[(\w+)\] (.*)", line)
            if m:
                results.append((m.group(2), m.group(3), m.group(1) == "PASS", ""))
        if not re.search(r"RESULT: \d+ checks", text):
            results.append(("lane%d" % k, "lane ran to the end (exit %s, see %s)" % (p.returncode, d / "out.txt"), False, ""))
        lj = d / "lane.json"
        if lj.exists():
            j = json.loads(lj.read_text(encoding="utf-8"))
            for kk, v in j["seen"].items():
                SEEN.setdefault(kk, v)
            for kk, v in j["used"].items():
                USED[kk] = USED.get(kk, 0) + v
            sections.update(j["sections"])
    (OUT / "sections.json").write_text(json.dumps(sections, indent=1), encoding="utf-8")
    CURRENT[0] = "coverage"
    whole = sorted(only) == sorted(f.__name__ for f in SECTIONS)
    unc = report_coverage() if whole else []
    failed = [r for r in results if not r[2]]
    log("RESULT: %d checks, %d failed%s" % (len(results), len(failed), ", %d uncovered" % len(unc) if whole else ""))
    for r in failed:
        log("  FAILED [%s] %s" % (r[0], r[1][:200]))
    sys.exit(1 if failed or unc else 0)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ensure_relay()   # the test clocks' own time server (run alone; under run_all_tests: its)
    args = sys.argv[1:]
    names = [f.__name__ for f in SECTIONS]
    if "--list" in args:
        import inspect
        for f in SECTIONS:
            print("%-20s %s" % (f.__name__, (f.__doc__ or "").strip().split("\n")[0]))
            parts = re.findall(r'@P\.part\("(\w+)"', inspect.getsource(f))
            if parts:
                print("%-20s parts: %s" % ("", " ".join(parts)))
        return
    only = names
    if "--only" in args:   # SECTION or SECTION.PART, comma-separated
        only = []
        for item in args[args.index("--only") + 1].split(","):
            sec, _, part = item.partition(".")
            if sec not in names:
                sys.exit("no section %s (--list)" % sec)
            only.append(sec)
            if part:
                ONLY_PARTS.setdefault(sec, []).append(part)
    if "--port-offset" in args:   # beside another run of the tests (run_all_tests.py --port-offset)
        global PORT_HOME, PORT_AP, PORT_CTL, HOME, AP, CTL
        off = int(args[args.index("--port-offset") + 1])
        PORT_OFF[0] = off
        PORT_HOME, PORT_AP, PORT_CTL = ports("web_ui", off)
        HOME, AP, CTL = ("http://127.0.0.1:%d" % p for p in (PORT_HOME, PORT_AP, PORT_CTL))
    if "--from" in args:   # the family's version (from_family; default sim/family_version.txt)
        FROM[0] = args[args.index("--from") + 1]
    if "--package" in args:
        PACKAGE[0] = args[args.index("--package") + 1]
    global OUT
    if "--out" in args:   # one lane of a --jobs run
        OUT = Path(args[args.index("--out") + 1])
    OUT.mkdir(parents=True, exist_ok=True)
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else 1
    if jobs > 1 and len(only) > 1 and not ONLY_PARTS:
        run_side_by_side(only, jobs, int(args[args.index("--port-offset") + 1]) if "--port-offset" in args else 0, args)
        return
    from playwright.sync_api import sync_playwright
    if "--js-coverage" in args:
        JSCOV[0] = JsCoverage(args[args.index("--js-coverage") + 1])
    FILESDIR[0] = make_files()
    clock = Clock()
    CLOCK[0] = clock
    try:
        if not wait(synced, 120, 1):
            sys.exit("the test clock did not come up synced - see %s" % (clock.dir / "run.log"))
        with sync_playwright() as pw:
            ui = UI(pw, show="--show" in args)
            ui._hook(ui.page)
            # a lane runs its sections in the order it was given (random, run_side_by_side); alone: as defined
            given = only if "--lane" in args else [f.__name__ for f in SECTIONS]
            for f in sorted(SECTIONS, key=lambda f: (f.__name__ in LAST_IN_LANE, given.index(f.__name__) if f.__name__ in given else 0)):
                if f.__name__ not in only:
                    continue
                CURRENT[0] = f.__name__
                t_sec = time.time()
                log("── %s: %s" % (f.__name__, (f.__doc__ or "").strip().split("\n")[0]))
                # Each section starts on a clock that has today's calendar - whatever the one
                # before did to its date (debug_time then chains, 2026-11-16)
                if not calendar_settled():
                    log("   (the clock has no downloaded calendar for today after 60 s - starting %s anyway)" % f.__name__)
                before = clock_snapshot()
                try:
                    f(ui, clock)
                except Exception as e:
                    check("section ran to the end", False, "%s: %s" % (type(e).__name__, e))
                    log(traceback.format_exc())
                clock.running_again()
                if JSCOV[0]:
                    JSCOV[0].take(f.__name__)
                if os.environ.get("JC_COV_DIR"):   # code coverage: the clock's counts so far under this section's name
                    try:
                        ctl("/coverage?name=web-ui." + f.__name__)
                    except Exception as e:
                        log("   (coverage counts not written: %s)" % e)
                left = snapshot_diff(before, clock_snapshot())
                if left:
                    log("   left changed by %s, put back: %s" % (f.__name__, "; ".join(left)))
                    put_back(before)
                    still = snapshot_diff(before, clock_snapshot())
                    if still:
                        log("   NOT PUT BACK after %s: %s" % (f.__name__, "; ".join(still)))
                harvest(ui.page)
                SECTION_S[f.__name__] = round(time.time() - t_sec)
            ui.close()
    finally:
        if "--keep" not in args:
            clock.stop()
    (OUT / "lane.json").write_text(json.dumps({"seen": SEEN, "used": USED, "sections": SECTION_S}, ensure_ascii=False), encoding="utf-8")
    if "--lane" in args:   # the parent reports coverage for the whole run
        failed = [r for r in results if not r[2]]
        log("RESULT: %d checks, %d failed" % (len(results), len(failed)))
        sys.exit(1 if failed else 0)
    CURRENT[0] = "coverage"
    whole = only == names and not ONLY_PARTS   # coverage only means something for a whole run
    unc = report_coverage() if whole else []
    failed = [r for r in results if not r[2]]
    log("RESULT: %d checks, %d failed%s" % (len(results), len(failed), ", %d uncovered" % len(unc) if whole else ""))
    for r in failed:
        log("  FAILED [%s] %s %s" % (r[0], r[1], r[3][:200]))
    sys.exit(1 if failed or unc else 0)


if __name__ == "__main__":
    main()
