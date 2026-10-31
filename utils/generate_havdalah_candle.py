"""
Draw the Havdalah candle from scratch (replaces a picture made from a store
photo, 2026-11-01): one 3-strand braid, each strand a pair of thin strands
(6 wicks), narrowing over its top part, under one wide dancing flame fed by
the six wicks; the flame lights the top of the wax.

Writes one animated BMP per candidate (same conventions as the other
generators: 16bpp RGB565 BI_BITFIELDS, black background, frames side by
side, frame count in biXPelsPerMeter) plus enlarged PNG/GIF previews, to
C:\\pio3\\jc_clean\\havdalah\\ (local_dirs.py) - copy the chosen one to
Data/IMG/Havdala.bmp.

    python utils/generate_havdalah_candle.py            # all candidates
    python utils/generate_havdalah_candle.py J          # just one
    python utils/generate_havdalah_candle.py --h 96 J   # for a 96-row window

Make it exactly the display window's height (calibration H) - then the clock
shows it pixel for pixel; a taller picture is shrunk to fit (less sharp).

Proportions started from the original photo (TEMP/Havdala.bmp): flame about
as wide as the candle's top. The narrowing over the top 35 rows, the flame
height and its yellow-orange tint were then chosen by eye on the display
(2026-10-31 / 11-01) - see the TUNABLES.
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
FRAME_H = 100           # default; --h N. The final box's window height, once calibrated
FRAME_COUNT = 16
SHOULDER_GAP_ROWS = 1   # rows between the flame's top room and the wax (see WAX_TOP_Y)
SS = 3                  # supersampling per axis (smooth edges)
# The flame is the old picture's (one_flame_color(), below), unchanged in
# logic: a flame per wick, each 1.35 strand-widths wide, so they burn as one
# wide flame. Height per candidate (flame_rows); the old picture had 11.5 rows
# on a 10 px top, the photo 1.4 candle widths.
FLAME_WIDTH = 1.35      # flame half-width in wick gaps
# The old flame's core is near-white - on the display it looked white
# (2026-11-01). Each flame colour is scaled by this (R, G, B): white turns
# warm yellow, edges orange. Its blue base stays; the glow on the wax
# (GLOW) is separate and unchanged.
FLAME_TINT = (1.0, 0.85, 0.35)
GLOW_ROWS = 9           # the flame lights the wax below it over this many rows
GLOW = (255, 214, 150)  # warm light the very top is mixed toward...
GLOW_MIX = 0.55         # ...this far (fades to 0 at GLOW_ROWS)
LIGHT = (-0.45, -0.55, 0.70)   # from upper left, toward the viewer
PREVIEW_SCALE = 6

BLUE = (35, 65, 205)
WHITE = (232, 234, 240)

# One 3-strand plait, each strand a PAIR of thin strands (as the old picture,
# and real Havdalah candles: 6 wicks). Lobe i belongs to strand i % 3.
# taper: width at the very top; shoulder: rows over which it widens to full width
# (the user, 2026-10-31: from about the left white lobe down - ~35 rows);
# lobe_*: lobe length (x width) / thickness (x lobe_rows); flame_rows: flame height.
_J = dict(plaits=1, width_px=30, lobe_rows=10.0, strands=[BLUE, WHITE, BLUE], wicks=6,
          angle=32, lobe_len=0.42, lobe_thick=0.58, flame_rows=22.0, shoulder=35)
CANDIDATES = {
    "J": dict(_J, taper=0.60),
    "J2": dict(_J, taper=0.50),
}


# --- The flame of the previous Havdala picture, moved here unchanged when that
# picture (and its script, generate_havdalah_flame.py) was retired 2026-11-01:
# its candle came from a store photo we have no license for. ---

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


def norm(v):
    m = math.sqrt(sum(c * c for c in v))
    return tuple(c / m for c in v)


L = norm(LIGHT)


def taper(v, top, shoulder):
    """Width factor at v rows below the wax top: from `top` to full width
    over `shoulder` rows - rounded at the top, blending smoothly into the
    straight candle below (quarter sine)."""
    if v >= shoulder:
        return 1.0
    t = max(0.0, v / shoulder)
    return top + (1.0 - top) * math.sin(math.pi / 2 * t)


class Candle:
    def __init__(self, cfg):
        self.cfg = cfg
        self.frame_w = max(48, cfg["width_px"] + 16)
        self.cx = self.frame_w / 2.0
        self.plait_w = cfg["width_px"] / cfg["plaits"]
        self.left = self.cx - cfg["width_px"] / 2.0
        # lobes per plait: (cx, cy, angle, a, b, colour); later ones on top
        p = cfg["lobe_rows"]
        self.lobes = []
        for k in range(cfg["plaits"]):
            pc = self.left + (k + 0.5) * self.plait_w
            n = int((FRAME_H - WAX_TOP_Y) / (p / 2)) + 3
            for i in range(-2, n):
                side = 1 if i % 2 == 0 else -1
                if k % 2:
                    side = -side              # neighbours mirror: herringbone
                ang = math.radians(cfg.get("angle", 32)) * side  # "\" on the left, "/" on the right
                self.lobes.append((pc + side * self.plait_w * 0.19,
                                   WAX_TOP_Y + i * p / 2 + p * 0.25,
                                   ang, self.plait_w * cfg.get("lobe_len", 0.40), p * cfg.get("lobe_thick", 0.42),
                                   cfg["strands"][i % len(cfg["strands"])]))
        top_w = cfg["width_px"] * taper(0, cfg["taper"], cfg["shoulder"])
        n = cfg["wicks"]
        inset = top_w / n / 4
        lo, hi = self.cx - top_w / 2 + inset, self.cx + top_w / 2 - inset
        self.wicks = [lo + j * (hi - lo) / (n - 1) for j in range(n)]
        self.wick_gap = top_w / n

    def body(self, x, y):
        """Colour of the wax at float pixel (x, y), or None."""
        v = y - WAX_TOP_Y
        if v < 0:
            return None
        s = taper(v, self.cfg["taper"], self.cfg["shoulder"])
        u = self.cx + (x - self.cx) / s          # un-tapered coordinate
        if abs(u - self.cx) > self.cfg["width_px"] / 2:
            return None
        if v < 1.2:                               # soft rounded top edge
            edge = abs(u - self.cx) / (self.cfg["width_px"] / 2)
            if v < 1.2 * (edge ** 3):
                return None
        best = None
        for (lx, ly, ang, a, b, col) in self.lobes:
            dx, dy = u - lx, y - ly
            if abs(dy) > a + 1 or abs(dx) > a + 1:
                continue
            ca, sa = math.cos(ang), math.sin(ang)
            ex = (dx * ca + dy * sa) / a
            ey = (-dx * sa + dy * ca) / b
            r2 = ex * ex + ey * ey
            if r2 > 1.0:
                continue
            # a pair of strands side by side: each half of the lobe's
            # thickness is its own round strand, a groove between them
            half = abs(ey) * 2 - 1                # -1..1 across one strand
            ey_s = math.copysign(half, ey) if ey else half
            r2s = min(1.0, ex * ex + half * half)
            nz = math.sqrt(1.0 - r2s)
            nx = ex * ca - ey_s * sa              # normal back in image frame
            ny = ex * sa + ey_s * ca
            diff = max(0.0, nx * L[0] + ny * L[1] + nz * L[2])
            k = 0.18 + 0.85 * diff
            spec = 90 * diff ** 14
            k *= 0.55 + 0.45 * min(1.0, (1.0 - r2) * 4)   # dark groove at the rim
            k *= 0.6 + 0.4 * min(1.0, (1.0 - half * half) * 3)  # groove between the pair
            best = tuple(min(255, int(c * k + spec)) for c in col)
        if best and v < GLOW_ROWS:                # lit by the flame above
            g = (1.0 - v / GLOW_ROWS) ** 1.5
            m = GLOW_MIX * g
            best = tuple(int(c * (1 - m) + gl * m) for c, gl in zip(best, GLOW))
        return best

    def flame(self, x, y, t):
        """The old picture's flame (one_flame_color()): one rounded
        flame per wick, each much wider than the wick gap - together one wide
        dancing flame."""
        out = None
        for j, wx in enumerate(self.wicks):
            ph = t + j * 0.37
            hm = math.sin(ph * 2) * 1.3 + math.cos(ph * 3.3 + j) * 0.8
            base = WAX_TOP_Y + 0.5             # foot just inside the wax: no dark gap
            c = one_flame_color(x, y, wx, ph, base, base - (self.cfg["flame_rows"] + hm),
                                self.wick_gap * FLAME_WIDTH)
            if c:
                if c[0] >= c[2]:                  # not the blue base
                    c = tuple(int(v * k) for v, k in zip(c, FLAME_TINT))
                out = blend(out, c)
        return out

    def render(self, t, body_cache):
        frame = []
        for py in range(FRAME_H):
            row = []
            for px in range(self.frame_w):
                acc = [0, 0, 0]
                for sy in range(SS):
                    for sx in range(SS):
                        x = px + (sx + 0.5) / SS
                        y = py + (sy + 0.5) / SS
                        c = None
                        if y < WAX_TOP_Y + 2:
                            c = self.flame(x, y, t)
                        if c is None:
                            c = body_cache.get((px, py, sx, sy))
                        if c:
                            acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
                row.append(tuple(v // (SS * SS) for v in acc))
            frame.append(row)
        return frame

    def body_cache(self):
        cache = {}
        for py in range(WAX_TOP_Y - 1, FRAME_H):
            for px in range(self.frame_w):
                for sy in range(SS):
                    for sx in range(SS):
                        c = self.body(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                        if c:
                            cache[(px, py, sx, sy)] = c
        return cache


def pack_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def write_bmp(path, frames):
    fw = len(frames[0][0])
    total_w, total_h = fw * len(frames), FRAME_H
    pad = (4 - (total_w * 2) % 4) % 4
    pixel_size = (total_w * 2 + pad) * total_h
    off = 54 + 12
    data = bytearray()
    data += struct.pack("<2sIHHI", b"BM", off + pixel_size, 0, 0, off)
    data += struct.pack("<IIIHHIIIIII", 40, total_w, total_h, 1, 16, 3,
                        pixel_size, len(frames), 0, 0, 0)
    data += struct.pack("<III", 0xF800, 0x07E0, 0x001F)
    for y in range(total_h - 1, -1, -1):
        for f in frames:
            for (r, g, b) in f[y]:
                data += struct.pack("<H", pack_rgb565(r, g, b))
        data += b"\x00" * pad
    path.write_bytes(bytes(data))


def to_image(frame):
    im = Image.new("RGB", (len(frame[0]), len(frame)))
    im.putdata([p for row in frame for p in row])
    return im


def main():
    out_dir = local_dirs.LOCAL / "havdalah"
    out_dir.mkdir(parents=True, exist_ok=True)
    global FRAME_H, WAX_TOP_Y
    args = sys.argv[1:]
    if "--h" in args:
        i = args.index("--h")
        FRAME_H = int(args[i + 1])
        del args[i:i + 2]
    names = args or list(CANDIDATES)
    tiles = []
    for name in names:
        # wax starts below the flame's room (+ its dancing, + a gap)
        WAX_TOP_Y = int(CANDIDATES[name]["flame_rows"] + 3 + SHOULDER_GAP_ROWS)
        candle = Candle(CANDIDATES[name])
        cache = candle.body_cache()
        frames = [candle.render(f * 2 * math.pi / FRAME_COUNT, cache) for f in range(FRAME_COUNT)]
        write_bmp(out_dir / f"Havdala_{name}.bmp", frames)
        tiles.append((name, [to_image(f) for f in frames]))
        print(f"{name}: {candle.frame_w}x{FRAME_H}, {len(frames)} frames")
    keep = {f"Havdala_{n}.bmp" for n in CANDIDATES} | {"candidates.png", "candidates.gif"}
    for f in out_dir.iterdir():                 # superseded candidates' files
        if f.name not in keep:
            f.unlink()
    write_previews(out_dir, tiles)
    print(f"Previews: {out_dir / 'candidates.png'} and .gif")


def write_previews(out_dir, tiles):
    """ONE still sheet + ONE animation with every candidate side by side, at
    fixed names, overwritten each run (6x, and 2x underneath for scale)."""
    gap, label_h = 24, 40
    w = sum(fr[0].width * PREVIEW_SCALE for _, fr in tiles) + gap * (len(tiles) + 1)
    h = FRAME_H * PREVIEW_SCALE + label_h + gap + FRAME_H * 2 + gap
    sheets = []
    for k in range(FRAME_COUNT):
        sheet = Image.new("RGB", (w, h), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        x = gap
        for name, fr in tiles:
            im = fr[k]
            d.text((x + im.width * PREVIEW_SCALE // 2 - 6, 10), name, fill=(255, 255, 255))
            sheet.paste(im.resize((im.width * PREVIEW_SCALE, FRAME_H * PREVIEW_SCALE), Image.NEAREST), (x, label_h))
            sheet.paste(im.resize((im.width * 2, FRAME_H * 2), Image.NEAREST),
                        (x, label_h + FRAME_H * PREVIEW_SCALE + gap))
            x += im.width * PREVIEW_SCALE + gap
        sheets.append(sheet)
    save_replacing(out_dir / "candidates.png", lambda f: sheets[0].save(f, format="PNG"))
    save_replacing(out_dir / "candidates.gif", lambda f: sheets[0].save(
        f, format="GIF", save_all=True, append_images=sheets[1:], duration=80, loop=0))


def save_replacing(path, save):
    """Write next to the target, then swap it in - works while a viewer has
    the old one open (in most viewers)."""
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "wb") as f:
        save(f)
    os.replace(tmp, path)


if __name__ == "__main__":
    main()
