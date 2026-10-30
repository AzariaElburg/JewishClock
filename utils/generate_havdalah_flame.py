"""
Animate the Havdalah candle: keep the existing braided candle body + stand
pixel-for-pixel, replace only the (currently static) flame region at the top
with six small, rounded, independently-flickering flames - one per braided
strand's own wick, since this candle is six candlettes braided together and
each sprouts its own wick where the braid is cut off at the top.

Analysis of the source image (Data/IMG/Havdala.bmp, 50x106):
  - y = 0..15   : the existing static flame (replaced per frame below)
  - y = 16..105 : braided candle body + metal stand (copied unchanged,
                   identical in every output frame)
  - candle top span at y=16 (where the braid ends): x = [15, 29] (14px wide)

The raw non-black span at y=16 is x=[15,29], but several of those edge
pixels are faint antialiasing (brightness <80/255) that a human eye doesn't
register as part of the candle - thresholding those out gives the actually
*visible* top edge: x=[18,28] (10px). Five wicks (not six - one dropped to
narrow the overall flame, per feedback) are spread evenly across that
visible width, inset by half a candlette's radius on each side (so the
outer wicks sit a half-radius in from the candle's own visible edges, not
flush with them) - candlette diameter = 10/5, radius = that/2, inset =
radius/2 on each side.

The device's display window has zero vertical safety margin (calibY+calibH
== the panel's full physical height), so a 106px-tall icon has nothing to
spare against the enclosure's cutout - the flame tip and stand bottom were
both getting physically clipped on the real device. Fix: shorten the whole
image by removing 14 rows (image rows 54-67) from the middle of the
repeating braided-wax pattern - found by brute-force searching every
plausible splice point in that region for the one with the smallest
pixel-wise difference across the cut (14 rows = exactly 2 full repeats of
the braid's ~7-row diagonal period, so the splice is seamless). The
shorter total height then centers with natural margin on both top and
bottom (the firmware centers an icon shorter than the window automatically)
- no calibration change needed, flame/stand both untouched.

Output: same 50px-wide frame, N frames side by side, frame count stashed in
biXPelsPerMeter exactly like generate_dual_candles.py - same BMP conventions
(16bpp, BI_BITFIELDS RGB565, pure black background, bottom-up rows).
"""
import math
import struct
from PIL import Image

# The ORIGINAL single-frame asset, not Data/IMG/Havdala.bmp (already
# overwritten with our animated version) - re-source pristine pixels from
# the backup made before that swap.
# Paths relative to the repo (this script lives in utils/), not one PC.
from pathlib import Path
_ROOT = Path(__file__).resolve().parent.parent
SRC = _ROOT / "assets" / "images" / "Havdala_orig.bmp"   # source art: kept in git, never sent to the clock
OUT = _ROOT / "TEMP" / "havdalah_flame_anim.bmp"

SRC_FRAME_H = 106       # the original asset's own height
WAX_SPLICE_START = 54   # image rows [54,68) removed - found via brute-force
WAX_SPLICE_END = 68     # search for the most seamless cut in the braid
FRAME_W = 50
FRAME_H = SRC_FRAME_H - (WAX_SPLICE_END - WAX_SPLICE_START)  # 92
FRAME_COUNT = 16
FLAME_CUTOFF_Y = 16  # rows >= this are the untouched candle body/stand

WICK_COUNT = 5
# Visually-perceived top edge, not the raw non-black span - pixels dimmer
# than this at y=16 are antialiasing fuzz a human eye doesn't read as candle.
CANDLE_TOP_LEFT = 18.0
CANDLE_TOP_RIGHT = 28.0
CANDLETTE_DIAMETER = (CANDLE_TOP_RIGHT - CANDLE_TOP_LEFT) / WICK_COUNT
CANDLETTE_RADIUS = CANDLETTE_DIAMETER / 2.0
INSET = CANDLETTE_RADIUS / 2.0
WICK_LEFT = CANDLE_TOP_LEFT + INSET
WICK_RIGHT = CANDLE_TOP_RIGHT - INSET
WICK_XS = [WICK_LEFT + i * (WICK_RIGHT - WICK_LEFT) / (WICK_COUNT - 1) for i in range(WICK_COUNT)]

# Each flame's own max half-width - deliberately much wider than its own
# candlette: six 1-2px-wide slivers don't read as flames at all at this
# scale, and real flames are always wider than their wick. Generous enough
# that neighbours substantially overlap and merge into one continuous
# glowing base (how six close-set wicks actually look), while each wick's
# own peak still stands recognisably apart higher up.
FLAME_MAX_W = CANDLETTE_DIAMETER * 1.35


def pack_rgb565(r, g, b):
    r = max(0, min(255, r))
    g = max(0, min(255, g))
    b = max(0, min(255, b))
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5


def load_source_rows():
    """Candle-body region (y>=FLAME_CUTOFF_Y) from the ORIGINAL asset, with
    the seamless wax splice removed, read once so every frame reuses it
    as-is."""
    im = Image.open(SRC).convert("RGB")
    assert im.size == (FRAME_W, SRC_FRAME_H), f"unexpected source size {im.size}"
    px = im.load()
    rows = []
    for y in range(FLAME_CUTOFF_Y, SRC_FRAME_H):
        if WAX_SPLICE_START <= y < WAX_SPLICE_END:
            continue  # the removed, seamlessly-matched chunk of wax
        rows.append([px[x, y] for x in range(FRAME_W)])
    assert len(rows) == FRAME_H - FLAME_CUTOFF_Y
    return rows


def one_flame_color(px_x, y, center_x, t, base_y, top_y, max_w):
    """A single small, rounded flame. Width follows an elliptical profile
    (not the sharp teardrop of a single large candle flame) - round bottom,
    round top, no straight tapering edges - appropriate for a flame this
    small. Returns (r,g,b) or None."""
    flame_len = base_y - top_y
    if not (top_y <= y <= base_y):
        return None
    fy = (base_y - y) / flame_len  # 0 at base, 1 at tip

    sway = math.sin(t) * 0.9
    curved_center = center_x + sway * fy

    # Ellipse-shaped width profile, bulged slightly toward the base rather
    # than perfectly centered, for a natural-looking rounded flame with no
    # sharp point at the tip.
    bulge_at = 0.38
    span_below, span_above = bulge_at, 1.0 - bulge_at
    norm = (fy - bulge_at) / span_below if fy < bulge_at else (fy - bulge_at) / span_above
    w = max_w * math.sqrt(max(0.0, 1.0 - norm * norm))
    if w < 0.25:
        return None

    dx = abs(px_x - curved_center)
    if dx > w + 1.0:
        return None
    dist = dx / max(0.001, w)

    if dist < 0.35 and fy < 0.6:
        if dist < 0.18 and fy < 0.05:
            return (20, 40, 220)          # blue base ignition - a thin sliver, not a patch
        return (255, 252, 230)            # white core
    if dist < 0.75:
        core_ratio = (dist - 0.35) / 0.40
        return (255, int(240 - 95 * core_ratio), int(90 - 90 * core_ratio))
    if dist < 1.0:
        edge_ratio = (dist - 0.75) / 0.25
        return (int(255 - 45 * edge_ratio), int(145 - 110 * edge_ratio), 10)
    # Beyond dist=1.0 this fades toward black - but blend() treats ANY
    # returned tuple (including literal black) as a real contribution to
    # merge with a neighbouring wick's color, which was the actual bug:
    # a faded-to-near-zero pixel here was silently halving a genuinely
    # bright neighboring flame pixel it got blended with, turning white/
    # orange into muddy grey. Must return None once there's nothing left
    # to show, not a (0,0,0) that looks like "no color" but isn't.
    # Short fade distance (was 0.4): a long lingering fade spends many
    # pixels in a dim, muddy brown that reads as dirt, not fire, at this
    # scale - cut from vivid to black over a much shorter span instead.
    fade = 1.0 - (dist - 1.0) / 0.15
    if fade <= 0.0:
        return None
    return (int(180 * fade), int(45 * fade), 0)


def blend(a, b):
    """Where two neighbouring flames' glow overlaps, take the per-channel
    max ("lighten") rather than averaging - averaging a bright white-core
    pixel with a dim fading-edge pixel desaturates toward muddy grey/tan,
    which read as the stray dark pixels reported; per-channel max always
    looks like more light overlapping, never less saturated."""
    if a is None:
        return b
    if b is None:
        return a
    return tuple(max(a[i], b[i]) for i in range(3))


def render_flame_row(y, t):
    row = [None] * FRAME_W
    for i, center_x in enumerate(WICK_XS):
        phase = t + i * 0.37          # each wick flickers slightly out of sync
        height_mod = math.sin(phase * 2) * 1.3 + math.cos(phase * 3.3 + i) * 0.8
        base_y = FLAME_CUTOFF_Y - 1.0
        top_y = base_y - (11.5 + height_mod)
        for x in range(FRAME_W):
            c = one_flame_color(x, y, center_x, phase, base_y, top_y, FLAME_MAX_W)
            if c:
                row[x] = blend(row[x], c)
    return [c if c else (0, 0, 0) for c in row]


def build_frame(body_rows, t):
    canvas = [render_flame_row(y, t) for y in range(FLAME_CUTOFF_Y)]
    canvas.extend(body_rows)
    return canvas


def write_bmp(frames):
    total_w = FRAME_W * FRAME_COUNT
    total_h = FRAME_H
    row_unpadded = total_w * 2
    padding = (4 - (row_unpadded % 4)) % 4
    row_total = row_unpadded + padding
    pixel_size = row_total * total_h

    header_size = 54
    masks_size = 12
    pixel_offset = header_size + masks_size
    file_size = pixel_offset + pixel_size

    file_header = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, pixel_offset)
    dib_header = struct.pack(
        "<IIIHHIIIIII", 40, total_w, total_h, 1, 16, 3,
        pixel_size, FRAME_COUNT, 0, 0, 0)
    masks = struct.pack("<III", 0xF800, 0x07E0, 0x001F)

    full = [[(0, 0, 0)] * total_w for _ in range(total_h)]
    for f_idx, frame in enumerate(frames):
        x0 = f_idx * FRAME_W
        for y in range(FRAME_H):
            for x in range(FRAME_W):
                full[y][x0 + x] = frame[y][x]

    pixel_bytes = bytearray()
    pad_bytes = b"\x00" * padding
    for y in range(total_h - 1, -1, -1):
        for x in range(total_w):
            r, g, b = full[y][x]
            pixel_bytes.extend(struct.pack("<H", pack_rgb565(r, g, b)))
        pixel_bytes.extend(pad_bytes)

    with open(OUT, "wb") as f:
        f.write(file_header)
        f.write(dib_header)
        f.write(masks)
        f.write(pixel_bytes)

    print(f"Generated: {OUT}")
    print(f"  Total Image Size : {total_w}x{total_h} px")
    print(f"  Single Frame Size: {FRAME_W}x{FRAME_H} px")
    print(f"  Frame Count      : {FRAME_COUNT} (in biXPelsPerMeter)")
    print(f"  Wick positions   : {[round(w, 2) for w in WICK_XS]}")


def main():
    body_rows = load_source_rows()
    frames = []
    for f in range(FRAME_COUNT):
        t = f * (2.0 * math.pi / FRAME_COUNT)
        frames.append(build_frame(body_rows, t))
    write_bmp(frames)


if __name__ == "__main__":
    main()
