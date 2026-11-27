"""
The printed manual: Data/manual.html of a release's tag, printed to one A4 PDF (2026-11-27) - the same
manual the clock serves and every update carries, its pictures inside it (utils/manual_shots.py). Its
fill-ins (the clock's texts, numbers, the update feed) are not asked from a clock here: the words in the
file are their fallbacks, equal to the clock's (manual_shots.py --check, run before stamping).

    C:/pio3/jc_clean/venv/Scripts/python utils/make_manual.py --version 2.2.0 [--out DIR]

Writes JewishClock-manual-<version>.pdf. (Until 2.1.2 this script wrote its own text; git has it.)
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
from local_dirs import DIST   # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", default=None, help="default: the release's dist folder")
    a = ap.parse_args()
    html = subprocess.run(["git", "show", "v%s:Data/manual.html" % a.version], cwd=ROOT, capture_output=True,
                          check=True).stdout.decode("utf-8")
    if ">%s</span>" % a.version not in html:
        sys.exit("Data/manual.html at v%s is not stamped with %s (utils/stamp_docs.py before the tag)" % (a.version, a.version))
    out = Path(a.out) if a.out else DIST / a.version
    out.mkdir(parents=True, exist_ok=True)
    src = out / ("manual-%s.html" % a.version)
    src.write_text(html, encoding="utf-8")
    pdf = out / ("JewishClock-manual-%s.pdf" % a.version)
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="msedge")
        pg = b.new_page()
        pg.goto(src.as_uri())
        pg.pdf(path=str(pdf), format="A4", print_background=True, margin={"top": "12mm", "bottom": "12mm", "left": "12mm", "right": "12mm"})
        b.close()
    src.unlink()
    print("OK:", pdf)


if __name__ == "__main__":
    main()
