"""
Draw the Torah scroll picture from scratch (replaces Data/IMG/Torah.bmp,
which came from a museum photo with no reuse license, 2026-10-31): two
parchment rolls on wooden rollers (turned handles, finials), a strip of
written parchment between them. Still picture, 16bpp RGB565 BMP like the
other generators. Candidates + enlarged previews to C:\\pio3\\jc_clean\\torah\\
(local_dirs.py) - copy the chosen one to Data/IMG/Torah.bmp.

    python utils/generate_torah.py          # all candidates
    python utils/generate_torah.py --h 96   # for a 96-row window

Make it exactly the display window's height (calibration H) - then the clock
shows it pixel for pixel; a taller picture is shrunk to fit (less sharp).
"""
import math
import os
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_dirs  # noqa: E402

# === TUNABLES ===
FRAME_H = 97            # default; --h N. The final box's window height (2026-11-04)
SS = 4                  # supersampling per axis
PREVIEW_SCALE = 6

PARCH = (238, 226, 196)
WOOD = (190, 128, 72)       # lighter after seeing it on the display (2026-11-01)
INK = (45, 35, 30)

# gap: px of open parchment between the rolls; roll_r: roll radius (px)
CANDIDATES = {
    "A5": dict(gap=24, roll_r=9.5, wood=WOOD),
}

# The open column, line by line - the Song of the Sea (Exodus 15) as in the
# original photo: full lines, a blank line, the song in its "brick" layout
# (lines of three pieces alternate with lines of two), blank, full lines.
# Each line: list of (from, to) ink spans as fractions of the column width.
FULL = [(0.0, 1.0)]
BRICK3 = [(0.0, 0.24), (0.38, 0.62), (0.76, 1.0)]
BRICK2 = [(0.0, 0.40), (0.60, 1.0)]
# Always these 14 lines (2 full, 4 verses of the song, 2 full): few lines
# with wide spacing, so the layout still reads at the display's resolution
# (chosen on the display 2026-11-01). A taller picture spaces them further apart.
PAGE = [FULL, FULL, []] + [BRICK3, BRICK2] * 4 + [[], FULL, FULL]
LINE_PITCH = None       # rows per line - whole rows (sharp), set in main()
INK_ROWS = 2            # ink rows per line; the rest of LINE_PITCH is space
TEXT_TOP_Y = None       # first line (a whole row), centred on the page, set in main()

HANDLE_SCALE = 0.6                  # top handle shortened: rows go to the page
HANDLE_SCALE_BOTTOM = 0.8           # bottom one a bit longer (rounded knob)
ROLL_TOP = 12.0                     # parchment rolls start (handle + plate above)
ROLL_BOTTOM_MARGIN = 15.5           # rows below the rolls (plate + handle)
ROLL_BOT = None                     # FRAME_H - ROLL_BOTTOM_MARGIN, set in main()
SHEET_INSET = 2.5                   # the open sheet is a bit shorter than the rolls
SIDE_MARGIN_PX = 1.5                # black beside the plates (was 3: halved 2026-11-01)


def shade(c, k, add=0):
    return tuple(max(0, min(255, int(v * k + add))) for v in c)


def cyl(dx, r):
    """Vertical cylinder lighting at offset dx from its axis (light from left)."""
    nx = max(-1.0, min(1.0, dx / r))
    nz = math.sqrt(max(0.0, 1 - nx * nx))
    diff = max(0.0, -0.55 * nx + 0.83 * nz)
    return 0.30 + 0.78 * diff, 60 * diff ** 18


def handle_radius(y, top):
    """Turned wooden handle profile, by row (top = the upper handle)."""
    if not top:
        y = FRAME_H - 0.5 - y          # mirror: measure from the bottom
    y = 0.5 + (y - 0.5) / (HANDLE_SCALE if top else HANDLE_SCALE_BOTTOM)
    if top:
        prof = [(0.5, 1.6), (2.0, 2.8), (3.5, 1.8), (5.0, 2.6), (6.5, 1.3),
                (11.5, 1.3), (12.5, 2.2), (14.0, 1.6)]
        if y < 0.5 or y > 14.0:
            return 0
    else:
        # rounded knob at the very bottom, then the grip up to the plate
        prof = [(0.5, 0.9), (1.1, 1.9), (1.9, 2.6), (3.0, 2.9), (4.0, 2.6),
                (4.8, 1.5), (10.0, 2.2), (12.0, 2.4), (13.5, 1.4), (15.5, 1.6)]
        if y < 0.5 or y > 15.5:
            return 0
    for (y0, r0), (y1, r1) in zip(prof, prof[1:]):
        if y0 <= y <= y1:
            return r0 + (r1 - r0) * (y - y0) / (y1 - y0)
    return 0


def pixel(cfg, frame_w, x, y):
    r = cfg["roll_r"]
    cx = frame_w / 2
    axes = [cx - cfg["gap"] / 2 - r, cx + cfg["gap"] / 2 + r]
    # discs (the rollers' wooden plates) just above/below the parchment
    for ax in axes:
        for dy0, dy1 in ((ROLL_TOP - 3, ROLL_TOP), (ROLL_BOT, ROLL_BOT + 3)):
            if dy0 <= y < dy1 and abs(x - ax) <= r + 2.5:
                k, s = cyl(x - ax, r + 2.5)
                edge = 0.75 if (y - dy0 < 0.6 or dy1 - y < 0.6) else 1.0
                return shade(cfg["wood"], k * edge, s)
    # parchment rolls
    for ax in axes:
        if ROLL_TOP <= y < ROLL_BOT and abs(x - ax) <= r:
            k, s = cyl(x - ax, r)
            # faint spiral lines of the rolled layers
            layer = 0.93 if (abs(x - ax) / r) > 0.86 else 1.0
            return shade(PARCH, k * layer, s)
    # the open sheet between the rolls, with lines of text
    x0, x1 = axes[0] + r * 0.6, axes[1] - r * 0.6
    if x0 <= x <= x1 and ROLL_TOP + SHEET_INSET <= y < ROLL_BOT - SHEET_INSET:
        v = y - TEXT_TOP_Y
        inner = axes[0] + r + 1.2 <= x <= axes[1] - r - 1.2
        line = int(v // LINE_PITCH) if v >= 0 else -1
        if inner and 0 <= line < len(PAGE) and (v % LINE_PITCH) < INK_ROWS:
            tx0, tx1 = axes[0] + r + 1.2, axes[1] - r - 1.2
            f = (x - tx0) / (tx1 - tx0)
            for a, b in PAGE[line]:
                # words: ink with small gaps (only in full lines - the song's
                # pieces are single words or two)
                if a <= f <= b and (len(PAGE[line]) > 1 or (x * 1.7 + line * 3.1) % 7.0 < 5.8):
                    return INK
        return shade(PARCH, 0.97)
    # handles and finials, above and below each roll
    for ax in axes:
        for top in (True, False):
            hr = handle_radius(y, top)
            if hr and abs(x - ax) <= hr:
                k, s = cyl(x - ax, hr)
                return shade(cfg["wood"], k, s)
    return None


def render(cfg):
    r = cfg["roll_r"]
    frame_w = int(math.ceil(cfg["gap"] + 4 * r + 2 * 2.5 + 2 * SIDE_MARGIN_PX))
    frame = []
    for py in range(FRAME_H):
        row = []
        for px in range(frame_w):
            acc = [0, 0, 0]
            for sy in range(SS):
                for sx in range(SS):
                    c = pixel(cfg, frame_w, px + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                    if c:
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
            row.append(tuple(v // (SS * SS) for v in acc))
        frame.append(row)
    return frame


def pack_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def write_bmp(path, frame):
    w, h = len(frame[0]), len(frame)
    pad = (4 - (w * 2) % 4) % 4
    pixel_size = (w * 2 + pad) * h
    off = 54 + 12
    data = bytearray()
    data += struct.pack("<2sIHHI", b"BM", off + pixel_size, 0, 0, off)
    data += struct.pack("<IIIHHIIIIII", 40, w, h, 1, 16, 3, pixel_size, 0, 0, 0, 0)
    data += struct.pack("<III", 0xF800, 0x07E0, 0x001F)
    for y in range(h - 1, -1, -1):
        for (r, g, b) in frame[y]:
            data += struct.pack("<H", pack_rgb565(r, g, b))
        data += b"\x00" * pad
    path.write_bytes(bytes(data))


def main():
    global FRAME_H, ROLL_BOT, LINE_PITCH, TEXT_TOP_Y
    args = sys.argv[1:]
    if "--h" in args:
        i = args.index("--h")
        FRAME_H = int(args[i + 1])
        del args[i:i + 2]
    ROLL_BOT = FRAME_H - ROLL_BOTTOM_MARGIN
    page_top, page_bot = ROLL_TOP + SHEET_INSET + 1.5, ROLL_BOT - SHEET_INSET - 1.5
    LINE_PITCH = max(2, int((page_bot - page_top) // len(PAGE)))
    TEXT_TOP_Y = int(page_top + ((page_bot - page_top) - LINE_PITCH * len(PAGE) + 1) / 2)
    out_dir = local_dirs.LOCAL / "torah"
    out_dir.mkdir(parents=True, exist_ok=True)
    tiles = []
    for name in args or list(CANDIDATES):
        frame = render(CANDIDATES[name])
        write_bmp(out_dir / f"Torah_{name}.bmp", frame)
        im = Image.new("RGB", (len(frame[0]), FRAME_H))
        im.putdata([p for row in frame for p in row])
        tiles.append((name, im))
        print(f"{name}: {im.width}x{FRAME_H}, lines {LINE_PITCH} rows ({INK_ROWS} ink)")
    keep = {f"Torah_{n}.bmp" for n in CANDIDATES} | {"candidates.png"}
    for f in out_dir.iterdir():                 # superseded candidates' files
        if f.name not in keep:
            f.unlink()
    # the clock's current picture first, for comparison (at its real pixels)
    now = Image.open(Path(__file__).resolve().parent.parent / "Data" / "IMG" / "Torah.bmp").convert("RGB")
    tiles.insert(0, ("now", now))
    gap, label_h = 24, 40
    hmax = max(t.height for _, t in tiles)
    w = sum(t.width * PREVIEW_SCALE for _, t in tiles) + gap * (len(tiles) + 1)
    h = hmax * PREVIEW_SCALE + label_h + gap + hmax * 2 + gap
    sheet = Image.new("RGB", (w, h), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    x = gap
    for name, t in tiles:
        big = t.resize((t.width * PREVIEW_SCALE, t.height * PREVIEW_SCALE), Image.NEAREST)
        d.text((x, 10), f"{name} {t.width}x{t.height}", fill=(255, 255, 255))
        sheet.paste(big, (x, label_h))
        sheet.paste(t.resize((t.width * 2, t.height * 2), Image.NEAREST),
                    (x, label_h + hmax * PREVIEW_SCALE + gap))
        x += big.width + gap
    tmp = out_dir / "candidates.png.tmp"         # swap in: the viewer keeps one file
    sheet.save(tmp, format="PNG")
    os.replace(tmp, out_dir / "candidates.png")
    print(f"Preview: {out_dir / 'candidates.png'}")


if __name__ == "__main__":
    main()
