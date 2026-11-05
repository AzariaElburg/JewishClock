"""
Builds include/Font.h's glyphs from a free (OFL) font, antialiased.

The browser draws each character (utils/generate_font.html, run in headless
Edge - the PC's own font renderer, smooth edges included); this script
writes the result into Font.h as 4-bit coverage (16 grey levels) per
pixel. The font: chosen by the user from a side-by-side comparison
(2026-10-20: Assistant ExtraBold; 2026-10-21: Noto Sans Hebrew Bold).

    python utils/generate_font.py

Font files in assets/fonts/, licences in THIRD_PARTY_LICENSES/. Only free
(OFL) fonts may be built in - Windows' own fonts are not licensed for that.
Same characters, same order and the same total width as the old pixel font,
so lines keep their length; Hebrew letters as tall as the digits (HEBREW_H).
Needs Microsoft Edge (installed with Windows).
"""
import functools
import http.server
import json
import re
import subprocess
import sys
import threading
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_H_PATH = ROOT / "include" / "Font.h"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

# The font (2026-10-21: Noto Sans Hebrew Bold; before: Assistant ExtraBold
# 800, squeeze 0.747, file Assistant-Variable.ttf).
FONT_FILE = "assets/fonts/NotoSansHebrew-Variable.ttf"
FONT_CREDIT = ("Noto Sans Hebrew Bold (weight {weight}) by the Noto Project Authors -\n"
               "//   https://github.com/notofonts/hebrew | SIL Open Font License 1.1\n"
               "//   (THIRD_PARTY_LICENSES/NotoSansHebrew-OFL.txt).")
WEIGHT = 700          # Bold
LETTER_H = 40         # font size: the one where א is this tall, px (as the old pixel font); digits then 48
# Hebrew drawn taller than that, same width: as tall as the digits (the user,
# 2026-11-04 - digits towered over the letters). ל then reaches 60 rows above
# the baseline, the tails of ן ק ך ף ץ 15 below.
HEBREW_H = 48
ROWS = 80             # cell height (FONT_H): room for ל (60 above the baseline) and | g (16 below)
BASE = 62             # baseline: the row under the letters' last ink row
COLON_SCALE = 0.7     # the colon drawn at 70%: full-size dots looked too big (user, 2026-10-20)
# Horizontal squeeze: the font drawn this narrow makes the Hebrew letters as
# wide in total as the old pixel font's, so lines keep their length. None =
# measure it against the old pixel font (OLD_FONT_REV), then fix it here so
# reruns don't depend on git history.
SQUEEZE = 0.7437      # Noto Sans Hebrew Bold, measured 2026-10-21
OLD_FONT_REV = "5f9191f^"   # the last commit with the 1-bit pixel font


def glyph_list(src):
    """(index, codepoint, label) for every glyph, in Font.h's order."""
    return [(int(m.group(1)), int(m.group(2), 16), m.group(3))
            for m in re.finditer(r"//\s*\[\s*(\d+)\]\s+U\+([0-9A-Fa-f]{4})\s+(.*)", src)]


def old_ink_widths(src):
    """Ink widths of the Hebrew letters in an old 1-bit Font.h (32 px rows)."""
    lines = src.split("\n")
    widths = {}
    for i, line in enumerate(lines):
        m = re.match(r"\s*//\s*\[\s*\d+\]\s+U\+(05[DE][0-9A-F])\s", line)
        if not m:
            continue
        cols = set()
        for k in range(64):
            vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", lines[i + 1 + k])]
            bits = (vals[0] << 24) | (vals[1] << 16) | (vals[2] << 8) | vals[3]
            cols |= {c for c in range(32) if bits & (0x80000000 >> c)}
        if cols:
            widths[int(m.group(1), 16)] = max(cols) - min(cols) + 1
    return widths


def render(cps, old_ink):
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 8794), functools.partial(Quiet, directory=str(ROOT)))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    q = {"font": FONT_FILE, "weight": WEIGHT, "letterH": LETTER_H, "hebrewH": HEBREW_H, "base": BASE, "rows": ROWS,
         "colonScale": COLON_SCALE,
         "cps": ",".join(map(str, cps)), "oldInk": json.dumps(old_ink)}
    if SQUEEZE is not None:
        q["sx"] = SQUEEZE
    url = "http://127.0.0.1:8794/utils/generate_font.html?" + urllib.parse.urlencode(q)
    try:
        out = subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--virtual-time-budget=10000",
                              "--dump-dom", url], capture_output=True, timeout=120).stdout.decode("utf-8", "replace")
    finally:
        srv.shutdown()
    m = re.search(r'<pre id="out">(.*?)</pre>', out, re.S)
    if not m or not m.group(1).strip():
        sys.exit("the page produced nothing - is Edge installed, and the font file in assets/fonts?")
    return json.loads(m.group(1).replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"'))


HEADER = """// =============================================================================
// Font.h  —  JewishClock antialiased font, {fw}×{rows}, 16 grey levels
//
// Source: {credit}
//   Every character drawn by utils/generate_font.py at the size where א is
//   {letter_h} px tall (digits {digit_h}), baseline under row {last_row}, narrowed to
//   {squeeze:.4f} of its width (as wide as the earlier pixel font's); Hebrew then
//   stretched to {hebrew_h} px tall, same width; the colon at {colon:.0%} and centred
//   on the digits. Chosen by the project owner; regenerate with that script,
//   don't edit the data by hand.
//
// Glyph format: {fw} pixels wide x {rows} pixels tall, 4 bits per pixel = how
//   much of the pixel the character covers (0 = none, 15 = full) - edge pixels
//   in between are drawn blended, which makes the edges smooth.
//   Each glyph = {bpg} bytes ({row_bytes} bytes x {rows} rows); in each byte the
//   high nibble is the left pixel. Ink starts at column 0.
//
// Total: {count} glyphs (95 Latin + 30 Hebrew) = {kb:.1f} KB PROGMEM
// =============================================================================
"""


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    src = FONT_H_PATH.read_text(encoding="utf-8")
    nl = "\r\n" if "\r\n" in src else "\n"
    src = src.replace("\r\n", "\n")
    glyphs = glyph_list(src)
    old = {}
    if SQUEEZE is None:   # the old pixel font, from git history
        old_src = subprocess.run(["git", "show", OLD_FONT_REV + ":include/Font.h"], cwd=ROOT,
                                 capture_output=True).stdout.decode("utf-8").replace("\r\n", "\n")
        old = old_ink_widths(old_src)
        if not old:
            sys.exit("SQUEEZE is not set and the old pixel font (%s) could not be read from git" % OLD_FONT_REV)
    if not glyphs:
        sys.exit("no glyph list found in Font.h")
    res = render([cp for _, cp, _ in glyphs], old)
    squeeze = res["sx"]
    for w in res["warn"]:
        print("warning:", w)

    data = res["glyphs"]
    fw = max(g["w"] for g in data.values())
    fw += fw % 2
    row_bytes = fw // 2
    out = []
    for idx, cp, label in glyphs:
        g = data[str(cp)]
        out.append("    // [%3d]  U+%04X  %s" % (idx, cp, label))
        for y in range(ROWS):
            row = (g["rows"][y] if g["w"] else "").ljust(fw, "0")
            out.append("    " + ", ".join("0x" + row[k:k + 2].upper() for k in range(0, fw, 2)) + ",")
    body = "\n".join(out)

    bpg = row_bytes * ROWS
    header = HEADER.format(credit=FONT_CREDIT.format(weight=res["weight"]), fw=fw, rows=ROWS, weight=res["weight"],
                           letter_h=LETTER_H, hebrew_h=HEBREW_H, digit_h=res["digitH"], last_row=BASE - 1, squeeze=squeeze,
                           colon=res["colonScale"], bpg=bpg, row_bytes=row_bytes, count=len(glyphs),
                           kb=len(glyphs) * bpg / 1024)
    src = header + src[src.index("#pragma once"):]
    src = re.sub(r"#define FONT_W\s+\d+[^\n]*", "#define FONT_W  %d   // pixels wide (the widest glyph)" % fw, src)
    src = re.sub(r"#define FONT_H\s+\d+[^\n]*", "#define FONT_H  %d   // pixels tall" % ROWS, src)
    src = re.sub(r"#define FONT_BASELINE\s+\d+[^\n]*",
                 "#define FONT_BASELINE  %d   // row under the letters' and digits' last ink row" % BASE, src)
    src = re.sub(r"#define FONT_LETTER_H\s+\d+[^\n]*",
                 "#define FONT_LETTER_H  %d   // Hebrew letters and digits: ink rows above the baseline (ל: more)"
                 % max(HEBREW_H, res["digitH"]), src)
    src = re.sub(r"#define FONT_BYTES_PER_GLYPH\s+\d+[^\n]*",
                 "#define FONT_BYTES_PER_GLYPH  %d  // %d bytes/row (4 bits per pixel) x %d rows" % (bpg, row_bytes, ROWS), src)
    src = re.sub(r"// Raw (?:bitmap|glyph) data[^\n]*\n// DO NOT read directly[^\n]*\n// Layout:[^\n]*\n",
                 "// Raw glyph data - %d bytes per glyph, stored in program flash.\n"
                 "// DO NOT read directly - use fontGetGlyphPtr() / fontGetCoverage().\n"
                 "// Layout: row-major, %d bytes per row, 4 bits per pixel, high nibble = left.\n" % (bpg, row_bytes), src)
    start = src.index("const uint8_t FONT_DATA[] PROGMEM = {") + len("const uint8_t FONT_DATA[] PROGMEM = {")
    end = src.index("\n};", start)
    src = src[:start] + "\n" + body + src[end:]
    FONT_H_PATH.write_bytes(src.replace("\n", nl).encode("utf-8"))
    print("Font.h: %d glyphs, %dx%d, size %.2f px, squeeze %.4f, %d bytes" %
          (len(glyphs), fw, ROWS, res["size"], squeeze, len(glyphs) * bpg))


if __name__ == "__main__":
    main()
