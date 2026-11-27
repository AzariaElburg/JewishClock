"""
"What's new" for a release, printed: Data/whats_new.html of the release's tag - the page the clock
serves (Settings > Clock software > What's new, 2026-11-27: it ships with the version) - with only
the versions after the family's (`--from`), to one A4 PDF. The text is only in that page.

    C:/pio3/jc_clean/venv/Scripts/python utils/make_whats_new.py --version 2.2.1 [--from 2.2.0] [--out DIR]

Writes JewishClock-<version>-whats-new.pdf. (Until 2.2.0 the text and its screenshots were in this
script; git has them.)
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
from local_dirs import DIST   # noqa: E402

SECTION = re.compile(r'<section class="ver" data-version="([^"]+)">.*?</section>\s*', re.S)


def vkey(v):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--from", dest="frm", default=(ROOT / "sim" / "family_version.txt").read_text(encoding="utf-8").strip())
    ap.add_argument("--out", default=None, help="default: the release's dist folder")
    a = ap.parse_args()
    html = subprocess.run(["git", "show", "v%s:Data/whats_new.html" % a.version], cwd=ROOT, capture_output=True,
                          check=True).stdout.decode("utf-8")
    secs = SECTION.findall(html)
    if not secs or secs[0] != a.version or ">%s</span>" % a.version not in html:
        sys.exit("Data/whats_new.html at v%s: its newest section must be %s, stamped (utils/stamp_docs.py %s before the tag)"
                 % (a.version, a.version, a.version))
    # only the versions after the family's: the rest they already have
    html = SECTION.sub(lambda m: m.group(0) if vkey(m.group(1)) > vkey(a.frm) else "", html)
    html = html.replace("<h1>JewishClock - What's new</h1>",
                        "<h1>JewishClock %s - What's new</h1>" % a.version, 1)
    out = Path(a.out) if a.out else DIST / a.version
    out.mkdir(parents=True, exist_ok=True)
    src = out / ("whats-new-%s.html" % a.version)
    src.write_text(html, encoding="utf-8")
    pdf = out / ("JewishClock-%s-whats-new.pdf" % a.version)
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="msedge")
        pg = b.new_page()
        pg.goto(src.as_uri())
        pg.pdf(path=str(pdf), format="A4", print_background=True, margin={"top": "13mm", "bottom": "13mm", "left": "14mm", "right": "14mm"})
        b.close()
    src.unlink()
    print("OK:", pdf, "(versions after %s)" % a.frm)


if __name__ == "__main__":
    main()
