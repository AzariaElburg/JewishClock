"""What the parents do, done the way they do it: through the clock's own
Wi-Fi ("JewishClock", 192.168.4.1 - their router keeps devices apart), with
the clock's own pages in a browser (Edge, driven by Playwright). Run before
shipping; a pre-ship check, not a unit test.

    C:/pio3/jc_clean/venv/Scripts/python utils/ap_tests.py [--skip page,settings,update] [--package FILE]

Three trips onto the clock's network (utils/ap_trip.py - the PC always comes
back home, even on a failure; it has no internet while away):
  page     - the page loads (and in a phone-sized window), its files match
             Data/; a setting changed and changed back; Save Wi-Fi;
             an event added with a picture, edited, deleted; Excel export
             ("Both") and import of its events (nothing should change).
  settings - the exported file's settings imported: the clock restarts -
             does this PC find the clock's network again, does the page come back.
  update   - the newest package from local_dirs.DIST installed with the clock's
             update page, followed to "Done!" (the clock restarts on the way).
  empty    - factory reset, then the clock put back from the exported file
             on its setup page (setup Wi-Fi, 192.168.4.1/restore) - the strongest
             check that a backup really restores everything.
Before, between and after: the clock's settings, events and pictures over the
home network, compared with how they were at the start (differences listed;
nothing is put back automatically). Results: local_dirs.LOCAL/ap_tests/<time>/.
"""
import argparse
import gzip
import json
import re
import struct
import subprocess
import sys
import time
import traceback
import urllib.request
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ap_trip import CLOCK_AP, HOME, _connected_ssid, _join, _reachable, on_clock_ap  # noqa: E402
from local_dirs import DIST, LOCAL  # noqa: E402

PROJ = Path(__file__).resolve().parent.parent
HOME_CLOCK = ["http://jewishclock.local", "http://<clock-ip>"]
AP_URL = "http://192.168.4.1"   # the clock's own Wi-Fi (both the synced AP and the setup one)
# Settings the clock changes by itself (the room's light, learned - BrightnessManager.cpp): not compared.
LEARNED = {"nightPct", "roomLightPct"}


def use_simulator(home_port=8080, ap_port=8081):
    """--sim: the same trips against the full simulator (sim/build_clock.py,
    jcclock.exe) instead of the clock: its home side and its own Wi-Fi are two
    ports on this PC, so there is no Wi-Fi to switch - "joining" a network is
    waiting until that side answers (it's down while the simulated clock
    restarts, as the clock's Wi-Fi is)."""
    global HOME_CLOCK, AP_URL, on_clock_ap, _connected_ssid, _join
    import contextlib
    HOME_CLOCK = ["http://127.0.0.1:%d" % home_port]
    AP_URL = "http://127.0.0.1:%d" % ap_port

    @contextlib.contextmanager
    def sim_ap():
        _join(CLOCK_AP, lambda: _reachable(AP_URL + "/api/status"), 120)
        yield AP_URL
    on_clock_ap = sim_ap
    _connected_ssid = lambda: CLOCK_AP   # a phone/PC that goes back to the clock's Wi-Fi by itself

    def in_setup_mode():
        # Only the setup page answers /wifi-scan (WiFiManager.cpp, AP portal);
        # the normal page redirects it. On the clock the setup network itself
        # only exists then - here the clock's Wi-Fi side answers all along, and
        # a restore sent before the factory reset had happened was erased by it.
        try:
            with urllib.request.urlopen(AP_URL + "/wifi-scan", timeout=4) as r:
                return r.status == 200 and b"networks" in r.read()
        except Exception:
            return False

    def sim_join(ssid, check, limit_s=60):
        t0 = time.time()
        while time.time() - t0 < limit_s:
            if check() and (not ssid.endswith("Setup") or in_setup_mode()):
                return time.time() - t0
            time.sleep(1)
        raise RuntimeError("%s did not answer within %d s" % (ssid, limit_s))
    _join = sim_join
OUT = LOCAL / "ap_tests" / time.strftime("%Y%m%d_%H%M%S")
results = []   # (stage, ok, detail)


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(OUT / "log.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def result(stage, ok, detail=""):
    results.append((stage, ok, detail))
    log("%s %s %s" % ("PASS" if ok else "FAIL", stage, detail))
    (OUT / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")


def get(base, path, timeout=30):
    """The file as the browser sees it: the page files come gzipped (utils/web_files.py)."""
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        data = r.read()
        return gzip.decompress(data) if r.headers.get("Content-Encoding") == "gzip" else data


def home_base():
    for _ in range(40):
        for b in HOME_CLOCK:
            if _reachable(b + "/api/status", 5):
                return b
        time.sleep(5)
    raise RuntimeError("clock not reachable over the home network")


def snapshot(tag):
    """Settings, events and pictures over the home network, saved as <tag>.json."""
    b = home_base()
    backup = json.loads(get(b, "/api/backup", 60))
    images = sorted(x["id"] for x in json.loads(get(b, "/api/images/list", 60)))
    status = json.loads(get(b, "/api/status"))
    snap = {"config": backup["config"], "events": backup["events"], "images": images,
            "firmware": status.get("firmware"), "updateState": status.get("updateState")}
    (OUT / (tag + ".json")).write_text(json.dumps(snap, ensure_ascii=False, indent=1), encoding="utf-8")
    return snap


def compare(before, tag, stage):
    try:
        after = snapshot(tag)
    except Exception as e:
        result(stage, False, "no snapshot over the home network: %s" % e)
        return None
    diffs = []
    for k in sorted(set(before["config"]) | set(after["config"])):
        if k in LEARNED:
            continue
        if before["config"].get(k) != after["config"].get(k):
            diffs.append("config.%s: %r -> %r" % (k, before["config"].get(k), after["config"].get(k)))
    # Without "id": an Excel import gives every event a new one (the file has none).
    ev = lambda s: sorted(json.dumps({k: v for k, v in e.items() if k != "id"}, sort_keys=True, ensure_ascii=False)
                          for e in s["events"])
    if ev(before) != ev(after):
        diffs.append("events: %d -> %d, %d differ" % (len(before["events"]), len(after["events"]),
                                                     len(set(ev(before)) ^ set(ev(after)))))
    if before["images"] != after["images"]:
        diffs.append("pictures: %s -> %s" % (before["images"], after["images"]))
    result(stage, not diffs, "; ".join(diffs) or "settings, events and pictures as at the start")
    return after


def png(w, h, seed):
    """A small PNG, different for each seed (so the clock sees a new picture)."""
    rows = b"".join(b"\x00" + b"".join(bytes(((x * 7 + seed) % 256, (y * 5 + seed * 3) % 256, ((x ^ y) + seed) % 256))
                                       for x in range(w))
                    for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def watch_page(pg, errors):
    pg.on("pageerror", lambda e: errors.append("page error: %s" % e))
    pg.on("console", lambda m: errors.append("console %s: %s" % (m.type, m.text)) if m.type == "error" else None)
    pg.on("requestfailed", lambda r: errors.append("request failed: %s %s" % (r.url, r.failure)))
    pg.on("dialog", lambda d: (errors.append("dialog (accepted): %s" % d.message), d.accept()))


def toast(pg):
    return pg.evaluate("el('toast').textContent")


def wait_until(fn, timeout_s, step_s=1.0):
    end = time.time() + timeout_s
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step_s)
    return None


# ── Trip 1: the page ─────────────────────────────────────────────────────────
def trip_page(url, browser):
    errors = []
    ctx = browser.new_context(viewport={"width": 1280, "height": 900}, accept_downloads=True)
    pg = ctx.new_page()
    pg.set_default_timeout(60000)
    watch_page(pg, errors)

    def stage(name, fn):
        try:
            fn()
        except Exception as e:
            result(name, False, "%s: %s" % (type(e).__name__, str(e).splitlines()[0] if str(e) else ""))
            log(traceback.format_exc())
            try:
                pg.screenshot(path=str(OUT / ("fail_" + name + ".png")))
            except Exception:
                pass

    def page_load():
        # Until its first time sync the clock answers "/" with its
        # "initializing..." page (WebUI.cpp) - a test straight after a start waits.
        wait_until(lambda: json.loads(get(url, "/api/status")).get("synced"), 120, 2)
        t0 = time.time()
        pg.goto(url + "/", wait_until="load")
        pg.wait_for_function("events.length > 0 && el('cfg-name').value !== ''", timeout=60000)
        took = time.time() - t0
        pg.screenshot(path=str(OUT / "calendar.png"))
        result("page loads", True, "%.1f s, %d events, name %r" % (took, pg.evaluate("events.length"), pg.evaluate("el('cfg-name').value")))
    stage("page loads", page_load)

    def files_match():
        bad = [p for p in ("index.html", "update.html", "sortable.min.js")
               if get(url, "/" + p if p != "index.html" else "/") != (PROJ / "Data" / p).read_bytes()]
        result("page files = Data/", not bad, "differ: %s" % bad if bad else "index.html, update.html, sortable.min.js")
    stage("page files = Data/", files_match)

    def setting():
        pg.click("button.rail-item[data-view=settings]")
        s0 = json.loads(get(url, "/api/config"))["scrollSpeed"]
        s1 = s0 + 1 if s0 < 10 else s0 - 1
        slide = "v => { const e = el('cfg-scroll-s'); e.value = v; e.dispatchEvent(new Event('input', { bubbles: true })); }"
        pg.evaluate(slide, s1)
        got = wait_until(lambda: json.loads(get(url, "/api/config"))["scrollSpeed"] == s1, 10)
        pg.evaluate(slide, s0)
        back = wait_until(lambda: json.loads(get(url, "/api/config"))["scrollSpeed"] == s0, 10)
        result("setting changed live", bool(got and back), "scroll speed %d -> %d -> %d" % (s0, s1, s0))
    stage("setting changed live", setting)

    def connectivity():
        pg.evaluate("el('toast').textContent = ''")
        pg.click("button:has-text('Save Wi-Fi')")
        t = wait_until(lambda: toast(pg), 15)
        result("Save Wi-Fi", t == "Saved", "toast %r" % t)
    stage("Save Wi-Fi", connectivity)

    name = "AP test %s" % time.strftime("%H%M%S")
    pic = OUT / "test_picture.png"
    pic.write_bytes(png(120, 160, int(time.time()) % 251))
    state = {}

    def event_add():
        pg.click("button.rail-item[data-view=calendar]")
        pg.click("button.fab")
        pg.fill("#ev-name", name)
        pg.select_option("#ev-type", "other")
        pg.fill("#ev-g-d", "15")
        pg.select_option("#ev-g-m", "3")
        pg.set_input_files("#ev-photo-file", str(pic))
        pg.wait_for_selector("#ev-photo-canvas:not(.hidden)", timeout=15000)
        pg.screenshot(path=str(OUT / "event_dialog.png"))
        pg.evaluate("el('toast').textContent = ''")
        pg.click("#modal-event .modal-footer button:has-text('Save')")
        pg.wait_for_function("!el('modal-event').classList.contains('open')", timeout=30000)
        # The dialog closes before the page has re-read its list (saveEvent).
        pg.wait_for_function("n => events.some(e => e.name === n)", arg=name, timeout=30000)
        ev = next((e for e in json.loads(get(url, "/api/events/list")) if e["name"] == name), None)
        images = [x["id"] for x in json.loads(get(url, "/api/images/list"))]
        state["ev"] = ev
        ok = bool(ev and ev.get("image") and ev["image"] in images)
        result("event added with picture", ok, "toast %r, image %s" % (toast(pg), ev and ev.get("image")))
    stage("event added with picture", event_add)

    def event_edit():
        ev = state["ev"]
        pg.evaluate("id => editEvent(events.findIndex(e => e.id === id))", ev["id"])
        pg.fill("#ev-name", name + " edited")
        pg.fill("#ev-warn", "2")
        pg.click("#modal-event .modal-footer button:has-text('Save')")
        pg.wait_for_function("!el('modal-event').classList.contains('open')", timeout=30000)
        pg.wait_for_function("n => events.some(e => e.name === n)", arg=name + " edited", timeout=30000)
        now = next((e for e in json.loads(get(url, "/api/events/list")) if e["id"] == ev["id"]), None)
        ok = bool(now and now["name"] == name + " edited" and now.get("warn") == 2 and now.get("image") == ev["image"])
        result("event edited", ok, "now %r, warn %s, picture kept: %s" % (now and now["name"], now and now.get("warn"),
                                                                       now and now.get("image") == ev["image"]))
    stage("event edited", event_edit)

    def event_delete():
        ev = state["ev"]
        pg.evaluate("id => editEvent(events.findIndex(e => e.id === id))", ev["id"])
        pg.click("#btn-del")
        pg.click("#cf-buttons button:has-text('Delete')")
        pg.wait_for_function("!el('modal-event').classList.contains('open')", timeout=30000)
        gone = all(e["id"] != ev["id"] for e in json.loads(get(url, "/api/events/list")))
        pic_gone = wait_until(lambda: ev["image"] not in [x["id"] for x in json.loads(get(url, "/api/images/list"))], 10)
        result("event deleted", gone, "its picture removed too: %s" % bool(pic_gone))
    stage("event deleted", event_delete)

    def export():
        pg.click("button.rail-item[data-view=settings]")
        pg.click("#scope-both")
        with pg.expect_download(timeout=120000) as dl:
            pg.click("button:has-text('Export')")
        path = OUT / dl.value.suggested_filename
        dl.value.save_as(str(path))
        state["xlsx"] = path
        result("Excel export (Both)", path.stat().st_size > 5000, "%s, %d bytes" % (path.name, path.stat().st_size))
    stage("Excel export (Both)", export)

    def import_events():
        pg.set_input_files("#data-upload-input", str(state["xlsx"]))
        pg.wait_for_selector("#data-result button:has-text('Import events')", timeout=30000)
        asked = pg.inner_text("#data-result")
        pg.click("#data-result button:has-text('Import events')")
        pg.wait_for_function("/Imported|not|failed/i.test(el('data-result').textContent) && !/Import events/.test(el('data-result').textContent)", timeout=120000)
        said = pg.inner_text("#data-result")
        pg.screenshot(path=str(OUT / "import_events.png"))
        ok = said.startswith("Imported") and "0 new, 0 changed, 0 removed" in said
        result("Excel import of its events", ok, "asked: %s | said: %s" % (asked.replace("\n", " ")[:200], said.replace("\n", " ")[:200]))
        pg.click("#data-result button:has-text('Close')")
    stage("Excel import of its events", import_events)

    ctx.close()
    # A phone-sized window - not a real phone, but the layout at that size.
    phone = browser.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=3, is_mobile=True, has_touch=True)
    pp = phone.new_page()
    watch_page(pp, errors)

    def phone_view():
        pp.goto(url + "/", wait_until="load")
        pp.wait_for_function("events.length > 0 && el('cfg-name').value !== ''", timeout=60000)
        pp.screenshot(path=str(OUT / "phone_calendar.png"), full_page=True)
        pp.click("button.rail-item[data-view=settings]")
        time.sleep(1)
        pp.screenshot(path=str(OUT / "phone_settings.png"), full_page=True)
        result("phone-sized page", True, "screenshots phone_calendar.png, phone_settings.png")
    try:
        phone_view()
    except Exception as e:
        result("phone-sized page", False, str(e).splitlines()[0])
    phone.close()
    real = [e for e in errors if "favicon" not in e]
    result("no page errors (trip 1)", not real, " | ".join(real)[:1500])


# ── Trip 2: import the exported settings (the clock restarts) ────────────────
def trip_settings(url, browser, xlsx):
    errors = []
    ctx = browser.new_context(viewport={"width": 1280, "height": 900})
    pg = ctx.new_page()
    pg.set_default_timeout(60000)
    watch_page(pg, errors)
    pg.goto(url + "/", wait_until="load")
    pg.wait_for_function("events.length > 0 && el('cfg-name').value !== ''", timeout=60000)
    pg.click("button.rail-item[data-view=settings]")
    pg.set_input_files("#data-upload-input", str(xlsx))
    pg.wait_for_selector("#data-result button:has-text('Import settings')", timeout=30000)
    pg.click("#data-result button:has-text('Import settings')")
    t0 = time.time()
    log("settings import started - the clock restarts")
    ssids, rejoined, reloaded = [], False, False
    while time.time() - t0 < 240:
        s = _connected_ssid()
        if not ssids or ssids[-1][1] != s:
            ssids.append((round(time.time() - t0), s))
            log("PC Wi-Fi at %+.0f s: %s" % (time.time() - t0, s))
        try:   # the page reloads itself once the clock answers again
            back = pg.evaluate("document.readyState === 'complete' && typeof events !== 'undefined' && events.length > 0 && !el('data-result').textContent.includes('Importing')")
        except Exception:
            back = False   # in the middle of the reload
        if back and time.time() - t0 > 10:
            reloaded = True
            break
        if time.time() - t0 > 90 and s != CLOCK_AP and not rejoined:
            log("PC not back on %s after 90 s - joining it by hand, as the parents would" % CLOCK_AP)
            _join(CLOCK_AP, lambda: _reachable(url + "/api/status"))
            rejoined = True
            try:
                pg.goto(url + "/", wait_until="load")
            except Exception:
                pass   # the page's own reload got there first - fine, the loop checks it
        time.sleep(2)
    pg.screenshot(path=str(OUT / "after_settings_import.png"))
    # Joining by hand is allowed (as in the update trip): Windows doesn't always
    # go back to the clock's Wi-Fi by itself; phones mostly do.
    result("settings import: page back", reloaded,
           "%.0f s; PC Wi-Fi: %s; joined by hand: %s" % (time.time() - t0, ssids, rejoined))
    ctx.close()


# ── Trip 3: the update page ──────────────────────────────────────────────────
def trip_update(url, browser, package):
    errors = []
    ctx = browser.new_context(viewport={"width": 600, "height": 900})
    pg = ctx.new_page()
    pg.set_default_timeout(60000)
    watch_page(pg, errors)
    pg.goto(url + "/update.html", wait_until="load")
    pg.wait_for_function("el('curVer').textContent !== 'checking...'", timeout=30000)
    before = pg.inner_text("#curVer")
    pg.set_input_files("#fileInput", str(package))
    pg.click("#goBtn")
    t0 = time.time()
    log("update started from %s with %s" % (before, package.name))
    last, ssids, rejoined, away_since = None, [], 0, None
    while time.time() - t0 < 15 * 60:
        s = _connected_ssid()
        if not ssids or ssids[-1][1] != s:
            ssids.append((round(time.time() - t0), s))
            log("PC Wi-Fi at %+.0f s: %s" % (time.time() - t0, s))
        away_since = None if s == CLOCK_AP else (away_since or time.time())
        if away_since and time.time() - away_since > 60:
            log("PC off %s for 60 s - joining it by hand, as the parents would" % CLOCK_AP)
            try:
                _join(CLOCK_AP, lambda: True, 60)
            except RuntimeError as e:
                log(str(e))
            rejoined += 1
            away_since = None
        try:
            cls, msg, bar = pg.evaluate("[el('msg').className, el('msg').textContent, el('bar').style.width]")
        except Exception as e:
            cls, msg, bar = "?", "page: %s" % e, ""
        if (cls, msg) != last:
            log("page at %+.0f s [%s %s]: %s" % (time.time() - t0, cls, bar, msg))
            last = (cls, msg)
        if cls in ("ok", "err") and "Sending" not in msg:
            break
        time.sleep(3)
    pg.screenshot(path=str(OUT / "update_page_end.png"))
    done = last and last[0] == "ok" and last[1].startswith("Done!")
    result("update page says Done", bool(done), "%.0f s; %s; PC Wi-Fi: %s; joined by hand: %d"
           % (time.time() - t0, last and last[1], ssids, rejoined))
    ctx.close()


# ── Trip 4: an empty clock put back from the backup file ─────────────────────
# Factory reset (over the home network), then what the family would do: join
# the clock's setup Wi-Fi, open 192.168.4.1/restore, choose the Excel file.
# The clock restarts and joins its Wi-Fi from the backup; then everything is
# compared with the start. The setup Wi-Fi's name and password come from
# Config.h (AP_SSID_PREFIX + "Setup", AP_PASSWORD_DEFAULT) - not written here.
def setup_ap():
    cfg = (PROJ / "include" / "Config.h").read_text(encoding="utf-8")
    prefix = re.search(r'#define AP_SSID_PREFIX\s+"([^"]*)"', cfg).group(1)
    pw = re.search(r'#define AP_PASSWORD_DEFAULT\s+"([^"]*)"', cfg).group(1)
    return prefix + "Setup", pw


def add_wifi_profile(ssid, pw):
    """A Windows Wi-Fi profile for the setup network (netsh can only join known ones)."""
    xml = ('<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">'
           '<name>%s</name><SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig><connectionType>ESS</connectionType>'
           '<connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>WPA2PSK</authentication>'
           '<encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey><keyType>passPhrase</keyType>'
           '<protected>false</protected><keyMaterial>%s</keyMaterial></sharedKey></security></MSM></WLANProfile>') % (ssid, ssid, pw)
    f = OUT / "setup_profile.xml"
    f.write_text(xml, encoding="utf-8")
    subprocess.run(["netsh", "wlan", "add", "profile", "filename=" + str(f)], capture_output=True, text=True)
    f.unlink()


def trip_empty(browser, xlsx):
    b = home_base()
    req = urllib.request.Request(b + "/api/factory-reset", data=b"body=%7B%7D", method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    said = urllib.request.urlopen(req, timeout=30).read().decode()
    log("factory reset: %s" % said)
    ssid, pw = setup_ap()
    url = AP_URL
    try:
        try:
            took = _join(ssid, lambda: _reachable(url + "/restore"), 180)
        except RuntimeError:
            add_wifi_profile(ssid, pw)
            took = _join(ssid, lambda: _reachable(url + "/restore"), 120)
        result("empty clock: setup Wi-Fi %s" % ssid, True, "joined after %.0f s" % took)
        # The setup page as served (reference for the simulator's - sim/compare_web.py).
        rec = OUT / "setup_page"
        rec.mkdir(exist_ok=True)
        for p in ("/", "/wifi-scan", "/restore", "/api/status", "/no/such/page"):
            try:
                req = urllib.request.Request(url + p, headers={"Accept-Encoding": "identity"})
                opener = urllib.request.build_opener(type("NoRedirect", (urllib.request.HTTPRedirectHandler,),
                                                          {"redirect_request": lambda *a, **k: None}))
                try:
                    r = opener.open(req, timeout=15); status, hdrs, body = r.status, dict(r.headers), r.read()
                except urllib.error.HTTPError as e:
                    status, hdrs, body = e.code, dict(e.headers), e.read()
                name = p.strip("/").replace("/", "_") or "root"
                (rec / (name + ".bin")).write_bytes(body)
                (rec / (name + ".json")).write_text(json.dumps({"path": p, "status": status, "headers": hdrs}, indent=1), encoding="utf-8")
            except Exception as e:
                log("setup page %s: %s" % (p, e))
        errors = []
        ctx = browser.new_context(viewport={"width": 600, "height": 900})
        pg = ctx.new_page()
        pg.set_default_timeout(60000)
        watch_page(pg, errors)
        pg.goto(url + "/restore", wait_until="load")
        pg.set_input_files("#restore-file", str(xlsx))
        msg = wait_until(lambda: (lambda t: t if re.search(r"^Restored:|not restored|not a backup|no usable|Damaged", t) else None)(
            pg.inner_text("#restore-msg")), 300, 2)
        pg.screenshot(path=str(OUT / "restore_page.png"))
        result("empty clock: restore page", bool(msg and msg.startswith("Restored:")), (msg or "no answer in 300 s").replace("\n", " | "))
        result("empty clock: restore names the clock's Wi-Fi", bool(msg and CLOCK_AP in msg and "choose the Wi-Fi" in msg),
               (msg or "").split("\n")[-1])
        real = [e for e in errors if "favicon" not in e and "request failed" not in e]
        result("no page errors (trip 4)", not real, " | ".join(real)[:1500])
        ctx.close()
    finally:
        for attempt in range(5):
            try:
                took = _join(HOME, lambda: _reachable("http://www.msftconnecttest.com/connecttest.txt"))
                log("back on %s after %.0f s" % (HOME, took))
                break
            except RuntimeError as e:
                log("%s (attempt %d)" % (e, attempt + 1))


def newest_package():
    return max(DIST.glob("*.jcupdate"), key=lambda p: p.stat().st_mtime)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip", default="", help="comma list: page,settings,update,empty")
    ap.add_argument("--package", help="the .jcupdate to install (default: newest in %s)" % DIST)
    ap.add_argument("--sim", action="store_true", help="against the full simulator (jcclock.exe on ports 8080/8081), not the clock")
    args = ap.parse_args()
    if args.sim:
        use_simulator()
    skip = set(filter(None, args.skip.split(",")))
    OUT.mkdir(parents=True, exist_ok=True)
    from playwright.sync_api import sync_playwright

    start = snapshot("before")
    log("start: firmware %s, %d events, %d pictures" % (start["firmware"], len(start["events"]), len(start["images"])))
    package = Path(args.package) if args.package else newest_package()
    new_version = package.stem.replace("JewishClock-update-", "")
    with sync_playwright() as p:
        if "page" not in skip:
            with on_clock_ap() as url:
                browser = p.chromium.launch(channel="msedge", headless=True)
                try:
                    trip_page(url, browser)
                finally:
                    browser.close()
            compare(start, "after_page", "home: unchanged after trip 1")
        xlsx = next(iter(sorted(OUT.glob("*.xlsx"))), None)
        if "settings" not in skip and xlsx:
            try:
                with on_clock_ap() as url:
                    browser = p.chromium.launch(channel="msedge", headless=True)
                    try:
                        trip_settings(url, browser, xlsx)
                    finally:
                        browser.close()
            except Exception as e:
                result("settings import", False, "%s: %s" % (type(e).__name__, e))
                log(traceback.format_exc())
            compare(start, "after_settings", "home: unchanged after settings import")
        if "update" not in skip:
            try:
                with on_clock_ap() as url:
                    browser = p.chromium.launch(channel="msedge", headless=True)
                    try:
                        trip_update(url, browser, package)
                    finally:
                        browser.close()
            except Exception as e:
                result("update", False, "%s: %s" % (type(e).__name__, e))
                log(traceback.format_exc())
            end = compare(start, "after_update", "home: unchanged after update")
            if end:
                result("home: new version running", end["firmware"] == new_version and end["updateState"] == "ok",
                       "%s (%s), expected %s" % (end["firmware"], end["updateState"], new_version))
        if "empty" not in skip and xlsx:
            try:
                browser = p.chromium.launch(channel="msedge", headless=True)
                try:
                    trip_empty(browser, xlsx)
                finally:
                    browser.close()
            except Exception as e:
                result("empty clock", False, "%s: %s" % (type(e).__name__, e))
                log(traceback.format_exc())
            compare(start, "after_restore", "home: everything back after factory reset + restore")
    fails = [r for r in results if not r[1]]
    log("== %d checks, %d failed ==" % (len(results), len(fails)))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
