"""
The printed install guide for an update package (2026-11-08): one A4 PDF in
English (the page's own words are English - Hebrew instructions mixed
right-to-left and left-to-right badly), each step with a screenshot of the
real page, the thing to click framed in red. Printed by the family, so no
flipping between the instructions and the page.

The screenshots come from a simulated clock at the family's version (its own
pages, the family's settings) that really installs the package through its
own Wi-Fi side (192.168.4.1 at the family's) - the page they will see.

    C:/pio3/jc_clean/venv/Scripts/python utils/make_install_guide.py --package FILE.jcupdate [--from 2.1.1] [--out DIR]

Needs that version's simulator build (C:/pio3/jc_clean/sim/versions/<from>/) and the
package's (sim/build_clock.py after its pio run). Writes
JewishClock-install-guide-<version>.pdf (+ the html and the screenshots).
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
import tarfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
sys.path.insert(0, str(ROOT / "sim"))
import web_files            # noqa: E402
from sim_ports import ports  # noqa: E402

WORK = Path("C:/pio3/jc_clean/sim/work/install_guide")
FAMILY_SEED = Path("C:/pio3/jc_clean/sim/family/seed")
RED = "e => { e.style.outline = '4px solid #e00000'; e.style.outlineOffset = '4px'; e.style.borderRadius = '6px'; }"
UNRED = "e => { e.style.outline = ''; e.style.outlineOffset = ''; }"


def http_json(url, timeout=5):
    return json.loads(urllib.request.urlopen(url, timeout=timeout).read())


def start_clock(version):
    """A clock at `version` with that version's pages and the family's data, on the guide's ports."""
    home, ap, ctl = ports("guide")
    web_files.remove_tree(WORK / "clock")
    tag = WORK / "tag"
    web_files.remove_tree(tag)
    tar = subprocess.run(["git", "archive", "v" + version, "Data"], cwd=ROOT, capture_output=True, check=True).stdout
    tarfile.open(fileobj=io.BytesIO(tar)).extractall(tag)
    fs = WORK / "clock" / "fs"
    web_files.stage(tag / "Data", fs)
    # The family's own settings, events and pictures as last synced (the family
    # simulator's seed): the page names their networks, as it will for them.
    seed = FAMILY_SEED if (FAMILY_SEED / "config.json").exists() else ROOT / "Data"
    for f in ("config.json", "events.json"):
        shutil.copy2(seed / f, fs / f)
    # At the family's the clock is on their own network (the first); the seed was
    # synced here before the handover, last on ours - the page names the network.
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg["wifiLastNetwork"] = 0
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    for sub in ("images", "custom"):
        if (seed / sub).is_dir():
            shutil.copytree(seed / sub, fs / sub, dirs_exist_ok=True)
    exe = Path("C:/pio3/jc_clean/sim/versions") / version / "jcclock.exe"
    if not exe.exists():
        sys.exit("no simulator build of %s: %s" % (version, exe))
    log = open(WORK / "clock" / "run.log", "wb")
    p = subprocess.Popen([str(exe), "--fs", str(fs), "--state", str(WORK / "clock" / "state"), "--verbose",
                          "--home-port", str(home), "--ap-port", str(ap), "--control-port", str(ctl)],
                         stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, JC_TITLE="Test (install guide)"))
    for _ in range(180):
        try:
            if http_json("http://127.0.0.1:%d/api/status" % home).get("synced"):
                return p
        except Exception:
            pass
        time.sleep(1)
    sys.exit("the clock didn't start - see %s" % (WORK / "clock" / "run.log"))


def stop_clock(p):
    try:
        urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/stop" % ports("guide")[2], data=b""), timeout=3)
    except Exception:
        pass
    time.sleep(2)
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)


def screenshots(package, shots):
    """Takes the steps' screenshots through the clock's own-Wi-Fi side; returns the version it ends on."""
    from playwright.sync_api import sync_playwright
    base = "http://127.0.0.1:%d" % ports("guide")[1]
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="msedge")
        pg = b.new_page(viewport={"width": 1100, "height": 720}, device_scale_factor=2)

        def shot(name, target=None, area=None, clip=None):
            if target:
                pg.locator(target).first.scroll_into_view_if_needed()
                pg.locator(target).first.evaluate(RED)
            pg.wait_for_timeout(400)
            path = shots / (name + ".png")
            if area:   # the area down to its last visible part, not its empty rest
                box = pg.locator(area).first.evaluate("""a => { const r = a.getBoundingClientRect();
                    let bottom = r.top + 40;
                    for (const e of a.querySelectorAll('*')) { const b = e.getBoundingClientRect();
                      if (b.height && getComputedStyle(e).visibility !== 'hidden') bottom = Math.max(bottom, b.bottom); }
                    return {x: r.left, y: r.top, width: r.width, height: Math.min(r.height, bottom - r.top + 18)}; }""")
                pg.screenshot(path=str(path), clip=box)
            else:
                pg.screenshot(path=str(path), clip=clip)
            if target:
                pg.locator(target).first.evaluate(UNRED)

        pg.goto(base + "/")
        pg.wait_for_timeout(3000)
        shot("open", target="button.rail-item[data-view=settings]", clip={"x": 0, "y": 0, "width": 760, "height": 300})
        pg.click("button.rail-item[data-view=settings]")
        pg.wait_for_timeout(1500)
        pg.fill("#settings-search", "update")
        pg.wait_for_timeout(1200)
        upd = 'a[href="/update.html"]'
        # the settings column with what the search left: from its head to the Update row
        head = pg.locator(".settings-head").bounding_box()
        row = pg.locator(upd).bounding_box()
        clip = {"x": max(0, head["x"] - 12), "y": max(0, head["y"] - 10),
                "width": min(1100 - max(0, head["x"] - 12), max(head["width"], row["x"] + row["width"] - head["x"]) + 30),
                "height": row["y"] + row["height"] - head["y"] + 30}
        pg.locator("#settings-search").evaluate(RED)
        pg.wait_for_timeout(300)
        pg.screenshot(path=str(shots / "search.png"), clip=clip)
        pg.locator("#settings-search").evaluate(UNRED)
        pg.locator(upd).evaluate(RED)
        pg.wait_for_timeout(300)
        pg.screenshot(path=str(shots / "settings.png"), clip=clip)
        pg.locator(upd).evaluate(UNRED)
        pg.click(upd)
        pg.wait_for_selector("#curVer")
        pg.wait_for_function("document.getElementById('curVer').textContent.trim() !== 'checking...'")
        shot("choose", target="#filePick", area=".card")
        pg.set_input_files("#fileInput", str(package))
        pg.wait_for_timeout(500)
        shot("update", target="#goBtn", area=".card")
        pg.click("#goBtn")
        pg.wait_for_function("/restarting|checks that it works/i.test(document.getElementById('msg').textContent)", timeout=120000)
        pg.wait_for_timeout(500)
        shot("wait", area=".card")
        pg.wait_for_function("/^Done/.test(document.getElementById('msg').textContent.trim())", timeout=480000)
        pg.wait_for_timeout(500)
        shot("done", target="#msg", area=".card")
        version = pg.locator("#msg").text_content()
        b.close()
    m = re.search(r"version (\S+?)\.?$", version.strip())
    return m.group(1) if m else ""


STEPS = [   # (screenshot or None, what to do, notes) - short bullets, the page's own words in <b> (the user, 2026-11-08)
    (None, "Save the update file attached to the email (its name ends in <b>.jcupdate</b>).",
     ["Do this first, while on your home Wi-Fi."]),
    (None, "Switch the computer's Wi-Fi to <b>{ap}</b>.",
     ["Password: on your printed start guide."]),
    ("open", "The clock's page opens. Click <b>Settings</b>.", []),
    ("search", "In <b>Search settings...</b> type <b>update</b>.", []),
    ("settings", "Click <b>Update the clock</b>.", []),
    ("choose", "Click <b>Tap to choose the update file</b> and choose the saved file.", []),
    ("update", "Click <b>Update</b>.", []),
    ("wait", "Wait about 2 minutes. Keep the page open.",
     ["If the computer switches back to your home Wi-Fi: switch to <b>{ap}</b> again."]),
    ("done", "When it says <b>Done!</b> - switch back to your home Wi-Fi.",
     ["Finished. Events and settings are as before."]),
]


def build_pdf(shots, version, ap, out_pdf):
    img = lambda n: "data:image/png;base64," + base64.b64encode((shots / (n + ".png")).read_bytes()).decode()
    items = []
    for name, line, notes in STEPS:
        items.append('<li><div class="do">%s</div>%s%s</li>' % (
            line.format(ap=ap),
            "<ul>%s</ul>" % "".join("<li>%s</li>" % n.format(ap=ap) for n in notes) if notes else "",
            '<img src="%s">' % img(name) if name else ""))
    html = """<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>Updating the Jewish Clock</title><style>
@page { size: A4; margin: 14mm 15mm; }
body { font-family: "Segoe UI", Arial, sans-serif; font-size: 14pt; color: #111; line-height: 1.3; }
h1 { font-size: 21pt; margin: 0 0 2mm; }
.sub { font-size: 12pt; color: #333; margin: 0 0 6mm; }
ol { padding-left: 9mm; margin: 0; }
ol > li { margin: 0 0 6mm; break-inside: avoid; font-weight: 700; font-size: 15pt; }
ol > li::marker { font-weight: 800; }
.do { font-weight: 400; font-size: 14pt; }
ul { margin: 1mm 0 0 0; padding-left: 6mm; font-weight: 400; font-size: 12.5pt; color: #333; }
img { display: block; margin-top: 2.5mm; max-width: 100%%; max-height: 80mm; border: 1px solid #999; border-radius: 2mm; }
.foot { font-size: 9.5pt; color: #555; margin-top: 2mm; }
</style></head><body>
<h1>Updating the Jewish Clock to version %s</h1>
<div class="sub">About 10 minutes. Click what is framed in <b style="color:#e00000">red</b>.</div>
<ol>
%s
</ol>
<div class="foot">If anything goes wrong, the clock goes back to the previous version by itself.</div>
</body></html>""" % (version, "\n".join(items))
    html_path = out_pdf.with_suffix(".html")
    html_path.write_text(html, encoding="utf-8")
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="msedge")
        pg = b.new_page()
        pg.goto(html_path.as_uri())
        pg.pdf(path=str(out_pdf), format="A4", print_background=True, prefer_css_page_size=True)
        b.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", required=True)
    ap.add_argument("--from", dest="frm", default=(ROOT / "sim" / "family_version.txt").read_text(encoding="utf-8").strip())
    ap.add_argument("--out", default=str(WORK / "out"))
    a = ap.parse_args()
    out = Path(a.out)
    shots = out / "screenshots"
    shots.mkdir(parents=True, exist_ok=True)
    seed = FAMILY_SEED if (FAMILY_SEED / "config.json").exists() else ROOT / "Data"
    ap_name = json.loads((seed / "config.json").read_text(encoding="utf-8")).get("projectName", "JewishClock")
    p = start_clock(a.frm)
    try:
        version = screenshots(Path(a.package).resolve(), shots)
    finally:
        stop_clock(p)
    if not version:
        sys.exit("the update didn't end with Done - see %s" % (WORK / "clock" / "run.log"))
    pdf = out / ("JewishClock-install-guide-%s.pdf" % version)
    build_pdf(shots, version, ap_name, pdf)
    print("OK:", pdf)


if __name__ == "__main__":
    main()
