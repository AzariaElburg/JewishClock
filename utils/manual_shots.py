"""
The screenshots of Data/manual.html, made again from the page as it is now (2026-11-26).

    C:/pio3/jc_clean/venv/Scripts/python utils/manual_shots.py            # shots + put them into Data/manual.html
    C:/pio3/jc_clean/venv/Scripts/python utils/manual_shots.py --check    # only: do the manual's <span class="ui"> names
                                                                    #       exist on the page? (no shots, no write)

What it does: starts a simulated clock of the working build (sim/build_clock.py's jcclock.exe) with the
working Data/ page files and made-up settings and events (no real network name, place or family
event - the pictures go out in every update package and to GitHub), takes each picture of the
manual with what to click framed in red, makes it small (palette PNG), and writes it into the
manual between `<!-- shot:NAME -->` and `<!-- /shot -->`, so the manual stays one file. A page
change -> run this again; nothing is copied by hand.

The same run checks every name the manual writes in <span class="ui"> against the page's own
words (all its text, labels, placeholders, titles, the settings' search entries): a name that is
not there is listed - the manual can't quietly say "Save Wi-Fi" after the page says something else.
Needs: playwright + Pillow (C:/pio3/jc_clean/venv), the dev build (sim/build_clock.py).
"""
import argparse
import base64
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
sys.path.insert(0, str(ROOT / "sim"))
import web_files                      # noqa: E402
from sim_ports import ports, no_real_feed   # noqa: E402

MANUAL = ROOT / "Data" / "manual.html"
WORK = Path("C:/pio3/jc_clean/sim/work/manual_shots")
EXE = Path("C:/pio3/jc_clean/sim/build/jcclock.exe")
RED = "e => { e.style.outline = '4px solid #e00000'; e.style.outlineOffset = '3px'; e.style.borderRadius = '6px'; }"
UNRED = "e => { e.style.outline = ''; e.style.outlineOffset = ''; }"
HOME, AP, CTL = ports("manual")
BASE = "http://127.0.0.1:%d" % HOME

# Made up: nothing of the family's. Everything else is the clock's own default.
SAMPLE_CONFIG = {
    "wifiSSID": "HomeNetwork", "wifiPassword": "sample-password", "wifiSSID2": "FamilyNetwork", "wifiPassword2": "sample-password",
    "wifiLastNetwork": 0, "projectName": "JewishClock", "setupComplete": True,
    "rotationMode": 0, "messageDwellSec": 4, "scrollSpeed": 2,
    # a name longer than the clock takes - for the "Too long ... OK, seen" picture (the clock keeps what fits)
    "wifiSSID3": "A-network-name-that-is-much-too-long-for-any-router-to-take-at-all",
    "wifiPassword3": "sample-password",
}
def sample_events():
    """Made up, a few days from today so the calendar's month shows them (what the page posts, persistEvent())."""
    import datetime
    t = datetime.date.today()
    def ev(name, kind, days, year, warn):
        d = t + datetime.timedelta(days=days)
        return {"name": name, "hasG": True, "hasJ": False, "isAdarII": False, "afterSunset": False, "icon": "", "type": kind,
                "jY": "", "jYNum": 0, "warn": warn, "gD": d.day, "gM": d.month, "gY": year, "jD": 0, "jMonth": ""}
    return [ev("Sarah's birthday", "birthday", 2, 1990, 2), ev("Wedding anniversary", "wedding", -3, 1985, 0),
            ev("Grandpa's yahrzeit", "yahrzeit", 6, 0, 1)]


# Words in the pictures that are the PC's or the build's, not the manual's: replaced in the page before each picture.
MASK_JS = r"""(city) => {
    const w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
    for (let n; (n = w.nextNode());) {
        let t = n.textContent;
        t = t.replace(/(password\s+)\S+/, '$1sample-1234');
        // the build's version and date: left out (the user, 2026-11-27 - a placeholder read oddly)
        t = t.replace(/\bv\d+\.\d+\.\d+.*/, '').replace(/built .*/, '');
        if (t !== n.textContent && /^[\s–-]*$/.test(t)) t = '';
        if (city && t.includes(city)) t = t.split(city).join('Jerusalem');
        if (t !== n.textContent) n.textContent = t;
    }
}"""


def http(url, data=None, timeout=10):
    """data: bytes, or an object as the page posts it (form field body=JSON)."""
    if data is not None and not isinstance(data, bytes):
        data = ("body=" + urllib.parse.quote(json.dumps(data))).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"} if data else {})
    return urllib.request.urlopen(req, timeout=timeout).read()


def start_clock():
    from time_relay import ensure_relay
    ensure_relay()
    for _ in range(2):
        try:
            http("http://127.0.0.1:%d/stop" % CTL, b"")
        except Exception:
            pass
    time.sleep(2)
    web_files.remove_tree(WORK)
    fs = WORK / "fs"
    web_files.stage(ROOT / "Data", fs)
    # not the project's own settings and events: made-up ones
    base = json.loads((ROOT.parent / "JewishClock_clean" / "Data" / "config.json").read_text(encoding="utf-8"))
    base.update(SAMPLE_CONFIG)
    (fs / "config.json").write_text(json.dumps(base, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (fs / "events.json").write_text("[]", encoding="utf-8")
    for p in (fs / "images", fs / "custom"):
        shutil.rmtree(p, ignore_errors=True)
    no_real_feed(fs)
    (WORK / "state").mkdir(parents=True)
    log = open(WORK / "run.log", "wb")
    shutil.copy2(EXE, WORK / "jcclock.exe")
    p = subprocess.Popen([str(WORK / "jcclock.exe"), "--fs", str(fs), "--state", str(WORK / "state"), "--verbose",
                          "--home-port", str(HOME), "--ap-port", str(AP), "--control-port", str(CTL)],
                         stdout=log, stderr=subprocess.STDOUT, cwd=str(WORK), env=dict(os.environ, JC_TITLE="Test (manual shots)"))
    for _ in range(180):
        try:
            if json.loads(http(BASE + "/api/status")).get("synced"):
                return p
        except Exception:
            pass
        time.sleep(1)
    sys.exit("the clock didn't start - see %s" % (WORK / "run.log"))


def stop_clock(p):
    try:
        http("http://127.0.0.1:%d/stop" % CTL, b"", 3)
    except Exception:
        pass
    time.sleep(2)
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)


def small_png(raw):
    """A palette PNG: the page is flat colours and text - 5-10x smaller than the screenshot, as sharp."""
    from PIL import Image
    im = Image.open(io.BytesIO(raw)).convert("RGB")
    im = im.quantize(colors=48, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    out = io.BytesIO()
    im.save(out, "PNG", optimize=True)
    return out.getvalue()


def page_words(pg):
    """Every word the page can show: text of all views, labels, placeholders, titles, options."""
    return pg.evaluate("""() => {
        const s = new Set();
        const add = t => { t = (t || '').replace(/\\s+/g, ' ').trim(); if (t) s.add(t); };
        for (const e of document.querySelectorAll('*')) {
            for (const n of e.childNodes) if (n.nodeType === 3) add(n.textContent);
            for (const a of ['placeholder', 'title', 'aria-label', 'value', 'alt']) add(e.getAttribute(a));
        }
        return [...s];
    }""")


def check_names(pg):
    """The manual's <span class="ui"> names against the page. Returns the ones not found."""
    html = re.sub(r"<!--.*?-->", "", MANUAL.read_text(encoding="utf-8"), flags=re.S)   # not the comments (their pictures, notes)
    names = sorted(set(re.sub(r"&amp;", "&", re.sub(r"<[^>]+>", "", n)).strip()
                       for n in re.findall(r'<span class="ui">(.*?)</span>', html, re.S)))
    words = page_words(pg)
    blob = "\n".join(words).lower()
    # the page's own scripts build more text (buttons made on the fly): the source counts too
    src = chr(10).join((ROOT / "Data" / f).read_text(encoding="utf-8").lower() for f in ("index.html", "update.html"))
    # what the clock fills in when the manual is viewed: its texts, tunables and Wi-Fi address - the words in
    # the page are only a fallback and must equal the clock's (no copy that can drift)
    texts = json.loads(http(BASE + "/api/texts"))
    tun = json.loads(http(BASE + "/api/tunables"))
    problems = []
    for m in re.finditer(r'data-text="(\w+)"[^>]*>([^<]*)<', html):
        want = texts.get(m.group(1))
        if want is None:
            problems.append("data-text %s: not a text of the clock" % m.group(1))
        elif want.replace("{}", "…") != m.group(2):
            problems.append("data-text %s: fallback %r, the clock says %r" % (m.group(1), m.group(2), want))
    for m in re.finditer(r'data-tunable="(\w+)"(?: data-fmt="(\w+)")?[^>]*>([^<]*)<', html):
        v = tun.get(m.group(1))
        if m.group(2) == "github":
            want = "github.com/%s/releases" % v
            if not isinstance(v, str) or m.group(3) != want or ('href="https://%s"' % want) not in m.group(0):
                problems.append("data-tunable %s: link %r, the clock says %r" % (m.group(1), m.group(3), v))
        elif not isinstance(v, (int, float)):
            problems.append("data-tunable %s: not a number the clock serves" % m.group(1))
    if not re.search(r"\d+(?:\.\d+){3}", texts.get("setupLine", "")):
        problems.append("data-apaddr: the clock's setupLine holds no address")
    # no number of the clock's typed into the text by hand: the words of a notice's days, "3 minutes", ...
    for m in re.finditer(r"\b(\d+|three|four|five|six|seven|ten|twenty)\s+(days?|minutes?|hours?|weeks?)\b", re.sub(r"<[^>]+>", " ", html), re.I):
        problems.append("a number typed in the text: %r (ask the clock: data-tunable)" % m.group(0))
    if problems:
        print("MANUAL PROBLEMS:" + chr(10) + "  " + (chr(10) + "  ").join(problems))
    missing = []
    for n in names:
        key = n.lower().replace("…", "...")
        if key in blob.replace("…", "...") or key in src.replace("…", "...") or key in src.replace("&amp;", "&"):
            continue
        missing.append(n)
    return missing


def shots(pg, only=None):
    """name -> png bytes."""
    out = {}

    city = (json.loads(http(BASE + "/api/config")).get("places") or [{}])[0].get("city", "")

    def shot_el(name, loc, mark=None, hide_head=True):
        """loc's picture; mark (a locator) framed in red. The settings' sticky search row is hidden."""
        loc.scroll_into_view_if_needed()
        if hide_head:
            pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = 'hidden')")
        pg.evaluate(MASK_JS, city)
        if mark is not None:
            mark.evaluate(RED)
        pg.wait_for_timeout(300)
        out[name] = small_png(loc.screenshot())
        if mark is not None:
            mark.evaluate(UNRED)
        pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = '')")

    def grp(title):
        return pg.locator(".grp").filter(has=pg.locator(".glabel .t", has_text=re.compile("^" + re.escape(title))))

    pg.set_viewport_size({"width": 1100, "height": 1300})
    pg.goto(BASE + "/")
    pg.wait_for_timeout(2500)
    # events (made up) through the clock's own call
    for ev in sample_events():
        http(BASE + "/api/events/upsert", ev)
    pg.reload()
    pg.wait_for_timeout(2500)

    # calendar, with the + that adds an event framed
    pg.set_viewport_size({"width": 1100, "height": 560})
    pg.wait_for_timeout(400)
    pg.locator(".fab").evaluate(RED)
    pg.wait_for_timeout(300)
    out["calendar"] = small_png(pg.screenshot(clip={"x": 132, "y": 40, "width": 968, "height": 520}))
    pg.locator(".fab").evaluate(UNRED)
    pg.set_viewport_size({"width": 1100, "height": 1300})
    pg.wait_for_timeout(400)
    pg.evaluate("openAddDialog()")
    pg.wait_for_timeout(1000)
    shot_el("event", pg.locator("#modal-event .modal-content"),
            pg.locator("#modal-event .modal-content button", has_text=re.compile("^Save$")), hide_head=False)
    pg.evaluate("closeModal()")
    pg.wait_for_timeout(400)

    pg.click("button.rail-item[data-view=settings]")
    pg.wait_for_timeout(1500)
    head = pg.locator(".settings-head")
    shot_el("search", head, pg.locator("#settings-search"), hide_head=False)
    shot_el("problems", pg.locator("#kept-texts"))
    # now the too-long network name goes (the wifi picture shows none)
    http(BASE + "/api/config", {"wifiSSID3": "", "wifiPassword3": ""})
    pg.reload()
    pg.wait_for_timeout(2500)
    pg.click("button.rail-item[data-view=settings]")
    pg.wait_for_timeout(1500)
    shot_el("display", grp("Display"))
    shot_el("content", grp("Calendar Content"))
    shot_el("wifi", grp("Wi-Fi"), pg.locator("button", has_text=re.compile("^Save Wi-Fi$")).first)
    shot_el("backup", grp("Export"), grp("Export").locator("button", has_text=re.compile("^Export$")))
    shot_el("software", grp("Clock software"), pg.locator('a[href="/update.html"]'))
    # the time servers: the rows of Advanced
    rows = pg.locator(".ntp-row")
    rows.first.scroll_into_view_if_needed()
    pg.evaluate(MASK_JS, city)
    a, z = rows.first.bounding_box(), rows.last.bounding_box()
    pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = 'hidden')")
    out["ntp"] = small_png(pg.screenshot(clip={"x": a["x"] - 10, "y": a["y"] - 6, "width": a["width"] + 20, "height": z["y"] + z["height"] - a["y"] + 14}))
    pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = '')")
    # the pictures: storage, event pictures and the first row of icons
    gb = pg.locator("#grp-builtin")
    gb.scroll_into_view_if_needed()
    pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = 'hidden')")
    g = gb.bounding_box()
    cut = pg.evaluate("""() => {
        const head = [...document.querySelectorAll('#grp-builtin .subhead')].find(h => /^Icons/.test(h.textContent.trim()));
        const grid = head && head.nextElementSibling;
        if (!grid || !grid.children.length) return null;
        const tops = [...grid.children].map(c => c.getBoundingClientRect().top);
        const second = tops.find(t => t > tops[0] + 10);
        return second == null ? null : second - 6; }""")
    if cut is None:
        cut = g["y"] + g["height"]
    out["pictures"] = small_png(pg.screenshot(clip={"x": g["x"], "y": g["y"], "width": g["width"], "height": cut - g["y"]}))
    pg.evaluate("document.querySelectorAll('.settings-head').forEach(e => e.style.visibility = '')")
    return out


ALT = {   # a picture's description (for screen readers and when the picture can't load)
    "problems": "Settings: a problem listed at the top, with the button OK, seen",
    "search": "Settings: the search box, framed",
    "calendar": "The calendar with the button that adds an event framed",
    "event": "The event window, Save framed",
    "pictures": "Settings: Pictures and icons",
    "display": "Settings: Display",
    "content": "Settings: Calendar Content",
    "wifi": "Settings: Wi-Fi, Save Wi-Fi framed",
    "backup": "Settings: Export and import, Export framed",
    "software": "Settings: Clock software, Update the clock framed",
    "ntp": "Settings: Advanced, the time servers",
}


def put_in_manual(png):
    """Each `<!-- shot:NAME -->...<!-- /shot -->` of the manual gets its picture (inline: the clock serves no other files)."""
    html = MANUAL.read_text(encoding="utf-8")
    marks = set(re.findall(r"<!-- shot:(\w+) -->", html))
    if marks - set(png):
        sys.exit("the manual wants pictures that were not made: %s" % sorted(marks - set(png)))
    def one(m):
        uri = "data:image/png;base64," + base64.b64encode(png[m.group(1)]).decode()
        return '<!-- shot:%s --><img class="shot" alt="%s" src="%s"><!-- /shot -->' % (m.group(1), ALT.get(m.group(1), m.group(1)), uri)
    new = re.sub(r"<!-- shot:(\w+) -->.*?<!-- /shot -->", one, html, flags=re.S)
    MANUAL.write_bytes(new.encode("utf-8"))
    print("pictures in the manual:", sorted(marks), "- not used by it:", sorted(set(png) - marks) or "none")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--save-dir", help="also keep the pictures as files here")
    a = ap.parse_args()
    p = start_clock()
    try:
        from playwright.sync_api import sync_playwright
        with sync_playwright() as pw:
            b = pw.chromium.launch(channel="msedge")
            pg = b.new_page(viewport={"width": 1100, "height": 1300}, device_scale_factor=1)
            pg.goto(BASE + "/")
            pg.wait_for_timeout(2500)
            missing = check_names(pg)
            print("names in the manual not found on the page:", missing or "none")
            if not a.check:
                png = shots(pg)
                if a.save_dir:
                    Path(a.save_dir).mkdir(parents=True, exist_ok=True)
                    for k, v in png.items():
                        (Path(a.save_dir) / (k + ".png")).write_bytes(v)
                for k, v in png.items():
                    print("%-10s %6d bytes" % (k, len(v)))
                put_in_manual(png)
            b.close()
    finally:
        stop_clock(p)


if __name__ == "__main__":
    main()
