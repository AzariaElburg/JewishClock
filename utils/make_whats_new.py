"""
"What's new" for a release, printed (2026-11-08): per change a title, one to
three short lines, and a screenshot of the new thing - a before/after only
where the difference is the point. No paragraphs (the user). The family's
settings; their previous version for "before", the new one for "now".

    C:/pio3/jc_clean/venv/Scripts/python utils/make_whats_new.py --version 2.2.0 [--from 2.1.1] [--out DIR]

Needs both versions' simulator builds and tags. Writes
JewishClock-<version>-whats-new.pdf. The texts below are this release's
(2.2.0); a later release replaces them.
"""
import argparse
import base64
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import make_install_guide as guide   # noqa: E402  (its work folder; start_clock / stop_clock for screenshots)

# (title, lines, screenshots) - screenshots: names, or ("before", "now") pairs, made by a release's own
# shots functions (2.1.2's are in git: shots_before / shots_now). 2.2.0 has none: a short list (the user).
NEWS = [
    ("New",
     ["The clock now looks for a new version on the internet once a day and installs it by itself at night.",
      "If you would rather decide yourself, turn off <b>Install new versions at night by themselves</b> in "
      "<b>Settings</b> &rarr; <b>Clock software</b>. A new version then waits there until you press <b>Install</b>.",
      "If an update does not work, the clock still goes back to its old version by itself. It now also tells you: "
      "a red line on the clock and a note in <b>Clock software</b> say which update failed, until you press <b>OK, seen</b>.",
      "When the clock loses its internet connection, it now shows the name and password of its own Wi-Fi every "
      "few rounds, so you can still open its page. It also shows them for the first few minutes after it is "
      "switched on.",
      "The password of the clock's own Wi-Fi can be changed in <b>Settings</b> &rarr; <b>Wi-Fi</b> &rarr; <b>Change</b>.",
      "When the clock finds a problem by itself, its screen shows <b>Error - see Settings</b>, and the top of "
      "<b>Settings</b> says what is wrong.",
      "The clock installs only updates that are signed by its maker."],
     []),
    ("Changed",
     ["When you change the order of the messages in <b>Calendar Content</b>, the clock follows the new order "
      "from its next message.",
      "Installing an update now restarts the clock once instead of twice.",
      "A name or text that is too long is now refused with a message saying why. Before, it was cut short without a word.",
      "Changing the clock's name now restarts the clock, so the new name is used everywhere at once.",
      "The Mishnah Yomit message has a new picture."],
     []),
    ("Fixed",
     ["The <b>24-hour clock</b> setting now changes the times on the clock (candle lighting, Havdalah, fasts). "
      "Before, it was saved but not used.",
      "An icon you make from an emoji or a few letters now shows on the clock. Before, it stayed blank.",
      "When the clock's storage was full, saving the events could lose all of them at the next restart. "
      "The clock now checks for room first, and tells you if there is none.",
      "After several weeks without a restart, the clock could stop setting its time. It now keeps it right "
      "however long it runs.",
      "After the clock got back onto the home Wi-Fi, its own Wi-Fi could keep the name it uses during setup."],
     []),
]


def build_pdf(shots, version, frm, out_pdf):
    img = lambda n: '<img src="data:image/png;base64,%s">' % base64.b64encode((shots / (n + ".png")).read_bytes()).decode()
    parts = []
    for title, lines, pics in NEWS:
        figs = []
        for p in pics:
            if isinstance(p, tuple):
                figs.append('<div class="pair"><div class="lab">Before (%s)</div>%s<div class="lab">Now (%s)</div>%s</div>' % (frm, img(p[0]), version, img(p[1])))
            else:
                figs.append(img(p))
        parts.append('<section><h2>%s</h2><ul>%s</ul>%s</section>' % (
            title, "".join("<li>%s</li>" % l for l in lines), "".join(figs)))
    html = """<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>Jewish Clock %s - what's new</title><style>
@page { size: A4; margin: 13mm 14mm; }
body { font-family: "Segoe UI", Arial, sans-serif; font-size: 12.5pt; color: #111; line-height: 1.25; }
h1 { font-size: 21pt; margin: 0 0 1mm; }
.sub { color: #444; margin: 0 0 5mm; font-size: 11.5pt; }
section { margin: 0 0 6mm; }
h2 { font-size: 15pt; margin: 0 0 1.5mm; padding-bottom: 1mm; border-bottom: 1.5px solid #333; break-after: avoid; }
ul { margin: 0 0 2mm; padding-left: 6mm; }
li { margin: 0 0 1mm; direction: ltr; }
img { display: block; max-width: 100%%; max-height: 95mm; margin: 1mm 0 3mm; border: 1px solid #999; border-radius: 2mm; break-inside: avoid; }
.pair { break-inside: avoid; }
.lab { font-size: 11pt; font-weight: 700; color: #444; margin-top: 1mm; }
.he { unicode-bidi: isolate; direction: rtl; }
</style></head><body>
<h1>Jewish Clock %s - what's new</h1>
<div class="sub">Changes since %s. Your events, pictures and settings stay as they are.</div>
%s
</body></html>""" % (version, version, frm, "\n".join(parts))
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
    ap.add_argument("--version", required=True)
    ap.add_argument("--from", dest="frm", default=(ROOT / "sim" / "family_version.txt").read_text(encoding="utf-8").strip())
    ap.add_argument("--out", default=str(guide.WORK.parent / "whats_new" / "out"))
    a = ap.parse_args()
    out = Path(a.out)
    shots = out / "screenshots"
    shots.mkdir(parents=True, exist_ok=True)
    if any(pics for _, _, pics in NEWS):
        sys.exit("NEWS has screenshots: add this release's shots functions (2.1.2's in git) and call them here")
    pdf = out / ("JewishClock-%s-whats-new.pdf" % a.version)
    build_pdf(shots, a.version, a.frm, pdf)
    print("OK:", pdf)


if __name__ == "__main__":
    main()
