"""
Draw the small Star of David glued after an event's age when its HEBREW date
brought it to the screen - only on events that have both a Hebrew and a
Gregorian date (2026-11-01; the Gregorian date gets no mark). The clock's
font has no U+2721, so it is a picture, like the event icons.

Still picture, 16bpp RGB565 BMP like the other generators. Candidates +
one enlarged preview to C:\\pio3\\jc_clean\\star\\ (local_dirs.py) - copy the
chosen one to Data/IMG/HebrewDate.bmp.

    python utils/generate_star.py
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
# Rows, tip to tip (--h). Sized to the TEXT, not the window (the user,
# 2026-11-04): letters and digits are 48 rows, brackets 59 - a star looks
# smaller than its outline, so a bit taller than the letters. (2026-11-01..04
# it filled the window: 85 rows; before that 34 looked too small.)
FRAME_H = 54
SIDE_PX = 3             # black at each side: room between star and text
SS = 6                  # supersampling per axis
PREVIEW_SCALE = 4

BLUE = (70, 120, 240)   # as the flag (lighter: dark blue vanishes on black)
CANDIDATES = {
    # stroke = line width in px, whatever the height: 6.5 chosen 2026-11-01
    # on the clock from 3.5 / 5 / 6.5 / 8; the font's strokes are 7-8 px.
    "I": dict(color=BLUE, stroke=6.5),
}


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def render(cfg):
    star_h, stroke = FRAME_H, cfg["stroke"]
    r = star_h / 2 - stroke / 2                    # triangles' corner radius (stroke centre)
    w = int(math.ceil(math.sqrt(3) * r + stroke)) + 2 * SIDE_PX
    h = star_h
    cx, cy = w / 2, star_h / 2
    tris = []
    for rot in (-90, 90):                          # point up, point down
        pts = [(cx + r * math.cos(math.radians(rot + k * 120)),
                cy + r * math.sin(math.radians(rot + k * 120))) for k in range(3)]
        tris.append(pts)
    frame = []
    for y in range(h):
        row = []
        for x in range(w):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    X, Y = x + (sx + 0.5) / SS, y + (sy + 0.5) / SS
                    for pts in tris:
                        if min(seg_dist(X, Y, *pts[i], *pts[(i + 1) % 3]) for i in range(3)) <= stroke / 2:
                            hits += 1
                            break
            k = hits / (SS * SS)
            row.append(tuple(int(c * k) for c in cfg["color"]))
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
    global FRAME_H
    if "--h" in sys.argv:
        FRAME_H = int(sys.argv[sys.argv.index("--h") + 1])
    out_dir = local_dirs.LOCAL / "star"
    out_dir.mkdir(parents=True, exist_ok=True)
    tiles = []
    for name, cfg in CANDIDATES.items():
        frame = render(cfg)
        write_bmp(out_dir / f"Star_{name}.bmp", frame)
        im = Image.new("RGB", (len(frame[0]), len(frame)))
        im.putdata([p for row in frame for p in row])
        tiles.append((name, im))
        print(f"{name}: {im.width}x{im.height}")
    keep = {f"Star_{n}.bmp" for n in CANDIDATES} | {"candidates.png"}
    for f in out_dir.iterdir():
        if f.name not in keep:
            f.unlink()
    gap, label_h = 24, 30
    h = max(t.height for _, t in tiles)
    w = sum(t.width * PREVIEW_SCALE for _, t in tiles) + gap * (len(tiles) + 1)
    sheet = Image.new("RGB", (w, h * PREVIEW_SCALE + label_h + gap + h * 2 + gap), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    x = gap
    for name, t in tiles:
        d.text((x, 8), f"{name} {t.width}x{t.height}", fill=(255, 255, 255))
        sheet.paste(t.resize((t.width * PREVIEW_SCALE, t.height * PREVIEW_SCALE), Image.NEAREST), (x, label_h))
        sheet.paste(t.resize((t.width * 2, t.height * 2), Image.NEAREST), (x, label_h + h * PREVIEW_SCALE + gap))
        x += t.width * PREVIEW_SCALE + gap
    tmp = out_dir / "candidates.png.tmp"
    sheet.save(tmp, format="PNG")
    os.replace(tmp, out_dir / "candidates.png")
    print(f"Preview: {out_dir / 'candidates.png'}")


if __name__ == "__main__":
    main()
