"""Draws the clock's event icons (the emoji an event can carry) as BMPs.

The clock's font has no emoji, so each emoji the event form offers gets a
picture in Data/IMG/ instead (the clock maps emoji -> file, see
MessageAggregator.cpp's eventIconPath()). Same format as Candles.bmp:
16-bit RGB565, BI_BITFIELDS, frames side by side, frame count in
biXPelsPerMeter. The flame is the one from generate_dual_candles.py.

    python utils/generate_event_icons.py            -> Data/IMG/*.bmp
    python utils/generate_event_icons.py --preview P -> also a PNG of all icons

Pure Python (no numpy), ~1 min. Drawn on black, the screen's background;
edges are smoothed by sampling each pixel 3x3 times.
"""
import math
import os
import struct
import sys
import zlib

HEIGHT = 96        # display window is 106 px tall; the font 64
SS = 3             # samples per pixel per axis (edge smoothing)
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Data", "IMG")


# ── shapes ───────────────────────────────────────────────────────────────────

def ellipse(x, y, cx, cy, rx, ry):
    return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


def rect(x, y, x0, y0, x1, y1):
    return x0 <= x <= x1 and y0 <= y <= y1


def round_rect(x, y, x0, y0, x1, y1, r):
    qx = max(x0 + r - x, 0, x - (x1 - r))
    qy = max(y0 + r - y, 0, y - (y1 - r))
    return x0 <= x <= x1 and y0 <= y <= y1 and qx * qx + qy * qy <= r * r


def polygon(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def seg_dist(x, y, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - ax - t * dx, y - ay - t * dy)


def scale(c, k):
    return (min(255, max(0, int(c[0] * k))), min(255, max(0, int(c[1] * k))), min(255, max(0, int(c[2] * k))))


def mix(a, b, k):
    return tuple(int(a[i] + (b[i] - a[i]) * k) for i in range(3))


def cyl(x, cx, half):
    """Brightness of a vertical cylinder at x: 1 in the middle, darker at the sides."""
    u = max(-1.0, min(1.0, (x - cx) / half))
    return 0.6 + 0.4 * math.cos(u * math.pi / 2)


def star_of_david(x, y, cx, cy, R, w):
    """Distance into the star's outline band (0..1 across the band) or None."""
    ri = R / 2.0
    best = None
    for normals in (((0, 1), (-0.866, -0.5), (0.866, -0.5)),
                    ((0, -1), (0.866, 0.5), (-0.866, 0.5))):
        sd = max(nx * (x - cx) + ny * (y - cy) for nx, ny in normals) - ri
        if -w <= sd <= 0:
            u = -sd / w
            best = u if best is None else max(best, u)
    return best


def flame(x, y, cx, base_y, height, max_w, t, phase=0.0):
    """The candle flame of generate_dual_candles.py, any size. t = 0..1 loop."""
    tt = t * 2 * math.pi + phase
    k = max(0.45, height / 56.0)
    sway = (math.sin(tt) * 2.5 + math.sin(tt * 2) * 1.0) * k
    top_y = base_y - height + (math.sin(tt * 2) * 2.5 + math.cos(tt * 4) * 1.0) * k
    if not (top_y <= y <= base_y):
        return None
    fy = (base_y - y) / (base_y - top_y)
    w = max_w * (fy / 0.22) ** 0.65 if fy < 0.22 else max_w * ((1.0 - fy) / 0.78) ** 1.15
    dx = abs(x - (cx + sway * fy ** 1.25))
    if dx > w + 2.5 * k:
        return None
    dist = dx / max(0.001, w)
    if dist < 0.32 and fy < 0.65:
        return (20, 40, 220) if fy < 0.18 else (255, 252, 230)
    if dist < 0.72:
        r = max(0.0, (dist - 0.32) / 0.40)                         # core in the upper flame too
        return (255, int(240 - 95 * r), int(90 - 90 * r))
    if dist < 1.0:
        r = (dist - 0.72) / 0.28
        return (int(255 - 45 * r), int(145 - 110 * r), 10)
    fade = max(0.0, 1.0 - (dist - 1.0) / 0.4)
    return (int(180 * fade), int(45 * fade), 0) if fade > 0 else None


def wick(x, y, cx, y0, y1):
    if abs(x - cx) <= 0.7 and y0 <= y <= y1:
        return (255, 90, 20) if y < y0 + 2 else (50, 35, 30)
    return None


# ── icons: shade(x, y, t) -> (r, g, b) or None (= black) ────────────────────

def cake(x, y, t):              # 🎂 two-tier cake, one candle
    c = flame(x, y, 38, 19, 17, 4.3, t) or wick(x, y, 38, 17, 22)
    if c:
        return c
    if abs(x - 38) <= 2.6 and 21 <= y <= 43:                       # candle, striped
        return (90, 160, 255) if int((x + y * 0.7) / 3) % 2 else (250, 250, 255)
    frosting, pink = (255, 250, 245), (245, 160, 190)
    for top, half, bottom in ((43, 20, 63), (63, 30, 87)):         # top tier, bottom tier
        if abs(x - 38) > half:
            continue
        curve = math.sqrt(max(0.0, 1 - ((x - 38) / half) ** 2))
        if ellipse(x, y, 38, top, half, 3.5):
            return frosting
        if top <= y <= bottom + 3 * curve:
            drip = 4 * max(0.0, math.cos((x - 38) * 2 * math.pi / 9)) ** 3
            if y <= top + 4 + drip:
                return scale(frosting, cyl(x, 38, half) + 0.1)
            col = pink
            if half == 30:                                         # sprinkles
                for sx, sy, sc in ((16, 74, (255, 220, 60)), (27, 80, (90, 200, 255)), (38, 73, (120, 220, 120)),
                                   (49, 81, (255, 220, 60)), (59, 74, (90, 200, 255)), (31, 72, (255, 255, 255))):
                    if abs(x - sx) <= 1.6 and abs(y - sy) <= 1.0:
                        col = sc
            return scale(col, cyl(x, 38, half))
    if ellipse(x, y, 38, 90, 36, 4):                               # plate
        return (215, 215, 225) if y < 90 else (150, 150, 165)
    return None


def memorial_candle(x, y, t):   # 🕯️ yahrzeit candle in a glass
    c = flame(x, y, 28, 48, 31, 6.5, t) or wick(x, y, 28, 46, 53)
    if c:
        return c
    half = 17 - (y - 38) * 2.0 / 54                                # glass narrows downwards
    dx = x - 28
    if 37 <= y <= 92 and abs(dx) <= half:
        if y >= 88:
            return (150, 170, 195)                                 # thick glass bottom
        if abs(dx) >= half - 2.2:
            return (165, 185, 210)                                 # glass walls
        if ellipse(x, y, 28, 53, half - 2.2, 3):
            return (255, 236, 200)                                 # wax top, lit by the flame
        if y >= 53:
            return scale((240, 232, 215), cyl(x, 28, half))        # wax
        if -11 <= dx <= -9:
            return (120, 135, 155)                                 # reflection on empty glass
        return (22, 26, 34)
    if ellipse(x, y, 28, 38, 17, 3) and not ellipse(x, y, 28, 38, 15, 1.8):
        return (200, 215, 235)                                     # rim
    return None


def rings(x, y, t):             # 💍 two wedding rings, one with a diamond
    s = max(0.0, math.sin(t * 2 * math.pi)) ** 2 * 8               # sparkle size
    dx, dy = abs(x - 40), abs(y - 26)
    if s > 0.5 and dx < s and dy < s and dx * dy < s * 0.5:
        return (255, 255, 255)
    # diamond on the left ring: crown above the girdle (y 36), pavilion below
    if polygon(x, y, ((26, 31), (34, 31), (38, 36), (22, 36))):
        return (235, 250, 255) if x < 30 else (170, 215, 245)
    if polygon(x, y, ((22, 36), (38, 36), (30, 46))):
        return (130, 190, 240) if x < 30 else (200, 235, 255)
    hits = []
    for cx, base in ((32, (235, 185, 70)), (52, (240, 200, 120))):
        d = math.hypot(x - cx, y - 66)
        if 16 <= d <= 22:
            u = (d - 16) / 6.0
            light = 0.5 + 0.5 * math.cos(math.atan2(y - 66, x - cx) + 2.35)
            col = scale(base, 0.55 + 0.45 * math.sin(math.pi * u) * (0.6 + 0.4 * light))
            if light > 0.75 and 0.35 < u < 0.65:
                col = mix(col, (255, 250, 230), 0.6)
            hits.append(col)
    if len(hits) == 2:                                             # interlocked: left in front above, right below
        return hits[0] if y < 66 else hits[1]
    if hits:
        return hits[0]
    if 28 <= x <= 36 and 44 <= y <= 46:
        return (235, 185, 70)                                      # setting
    return None


def star(x, y, t):              # ✡️
    u = star_of_david(x, y, 42, 50, 42, 6.5)
    if u is None:
        return None
    return scale(mix((40, 110, 235), (110, 170, 255), max(0.0, (60 - y) / 60)), 0.6 + 0.4 * math.sin(math.pi * u))


def synagogue(x, y, t):         # 🕍
    u = star_of_david(x, y, 44, 14, 8, 2.2)
    if u is not None:
        return (250, 205, 70)
    if abs(x - 44) <= 0.8 and 19 <= y <= 28:
        return (230, 180, 60)                                      # spire
    stone = (235, 215, 170)
    if y <= 46 and ellipse(x, y, 44, 46, 20, 19):                  # dome
        return scale((235, 180, 60), 0.6 + 0.4 * max(0.0, math.cos((x - 38) / 26 * math.pi / 2)))
    for tx in (13, 75):                                            # towers with small domes
        if y <= 32 and ellipse(x, y, tx, 32, 7.5, 8):
            return scale((235, 180, 60), cyl(x, tx - 2, 8))
        if abs(x - tx) <= 7.5 and 32 <= y <= 88:
            if abs(x - tx) <= 2.5 and (ellipse(x, y, tx, 44, 2.5, 2.5) or 44 <= y <= 56):
                return (90, 145, 215)                              # window
            return scale(stone, 0.85)
    if 20 <= x <= 68 and 46 <= y <= 88:
        if abs(x - 44) <= 6 and (ellipse(x, y, 44, 70, 6, 6) or 70 <= y):
            return (100, 60, 35)                                   # arched door
        for wx in (29, 59):
            if abs(x - wx) <= 3.5 and (ellipse(x, y, wx, 58, 3.5, 3.5) or 58 <= y <= 70):
                return (90, 145, 215)
        return stone
    if 2 <= x <= 86 and 88 <= y <= 93:
        return (170, 170, 178)                                     # steps
    return None


def party(x, y, t):             # 🎉 party popper with flying confetti
    colors = ((255, 80, 90), (80, 200, 255), (255, 220, 50), (120, 230, 120), (220, 120, 255), (255, 160, 60))
    for i in range(14):                                            # confetti: out of the mouth, then falling
        a = -1.45 + (i % 7) * 0.19 + (0.05 if i >= 7 else 0)       # launch angle (screen coords)
        p = (t + i / 14.0) % 1.0
        px = 44 + math.cos(a) * 70 * p
        py = 44 + math.sin(a) * 70 * p + 60 * p * p
        rot = (i + p * 6) * 1.3
        lx = (x - px) * math.cos(rot) + (y - py) * math.sin(rot)
        ly = -(x - px) * math.sin(rot) + (y - py) * math.cos(rot)
        if abs(lx) <= 1.6 and abs(ly) <= 2.8 and p > 0.04:
            return colors[i % len(colors)]
    tip, axis = (12.0, 88.0), (0.6, -0.8)                          # cone: tip lower left, opens up right
    vx, vy = x - tip[0], y - tip[1]
    along = vx * axis[0] + vy * axis[1]
    across = vx * -axis[1] + vy * axis[0]
    if 0 <= along <= 52 and abs(across) <= along * 0.32:
        if along > 49:
            return (90, 40, 30)                                    # mouth
        col = (250, 200, 40) if int(along / 7) % 2 else (230, 55, 70)
        return scale(col, 0.7 + 0.3 * math.cos(across / (along * 0.32 + 0.01) * math.pi / 2))
    return None


def baby(x, y, t):              # 👶
    skin = (255, 214, 170)
    d = math.hypot(x - 40, y - 54)
    if math.hypot(x - 42, y - 22) <= 6 and math.hypot(x - 42, y - 22) >= 3 and not (x > 42 and y > 22):
        return (160, 100, 50)                                      # curl of hair
    if d <= 32:
        if math.hypot(x - 40, y - 60) <= 10 and math.hypot(x - 40, y - 60) >= 8 and y > 64:
            return (210, 90, 90)                                   # smile
        for ex in (28, 52):
            if ellipse(x, y, ex + 1, 51, 1.3, 1.3):
                return (255, 255, 255)                             # eye shine
            if ellipse(x, y, ex, 53, 3.6, 4.6):
                return (60, 40, 30)
            if ellipse(x, y, ex - 6 if ex < 40 else ex + 6, 65, 6, 3.6):
                return mix(skin, (255, 140, 140), 0.55)            # cheeks
        return scale(skin, 0.75 + 0.25 * math.cos(min(1.0, d / 32) * math.pi / 2))
    for ex in (8, 72):
        if ellipse(x, y, ex, 56, 6, 8):
            return scale(skin, 0.85)                               # ears
    return None


def grad_cap(x, y, t):          # 🎓
    gold = (250, 200, 60)
    if seg_dist(x, y, 44, 40, 74, 47) <= 1.2 or seg_dist(x, y, 74, 47, 74, 70) <= 1.2:
        return gold                                                # tassel cord
    if 70 <= x <= 78 and 68 <= y <= 84:
        return gold if int(x) % 2 else (210, 160, 40)              # tassel
    if math.hypot(x - 44, y - 40) <= 3:
        return gold
    if polygon(x, y, ((44, 24), (86, 40), (44, 56), (2, 40))):     # board
        edge = min(seg_dist(x, y, 44, 24, 86, 40), seg_dist(x, y, 86, 40, 44, 56),
                   seg_dist(x, y, 44, 56, 2, 40), seg_dist(x, y, 2, 40, 44, 24))
        return (140, 160, 225) if edge < 1.5 else (55, 75, 160)
    if 22 <= x <= 66 and 46 <= y <= 62 + 4 * math.sqrt(max(0.0, 1 - ((x - 44) / 22) ** 2)):
        return scale((40, 55, 125), cyl(x, 44, 22))                # cap
    return None


def house(x, y, t):             # 🏠
    if 58 <= x <= 67 and 16 <= y <= 36:
        return (175, 85, 60)                                       # chimney
    if polygon(x, y, ((42, 12), (84, 50), (0, 50))):
        return (215, 65, 50) if int((y - 12) / 6) % 2 else (190, 50, 40)
    if 10 <= x <= 74 and 50 <= y <= 90:
        if 35 <= x <= 49 and 64 <= y:
            return (225, 185, 60) if math.hypot(x - 46, y - 78) <= 1.4 else (135, 80, 40)
        for wx in (22, 62):
            if abs(x - wx) <= 7 and abs(y - 66) <= 7:
                return (250, 250, 250) if abs(x - wx) <= 0.8 or abs(y - 66) <= 0.8 else (110, 170, 230)
        return (245, 230, 190)
    return None


def calendar(x, y, t):          # 🗓️
    for rx in (22, 54):
        if round_rect(x, y, rx - 2.5, 10, rx + 2.5, 24, 2):
            return (120, 120, 130)                                 # binder rings
    if not round_rect(x, y, 6, 16, 70, 88, 6):
        return None
    if y <= 32:
        return (225, 60, 60)
    for row in range(3):
        for col in range(4):
            cx0, cy0 = 13 + col * 14, 40 + row * 15
            if rect(x, y, cx0, cy0, cx0 + 9, cy0 + 9):
                return (225, 60, 60) if (row, col) == (1, 2) else (185, 185, 198)
    return (248, 248, 248)


# name, drawing, width, frames (1 = still picture)
ICONS = (
    ("Cake", cake, 76, 16),
    ("MemorialCandle", memorial_candle, 56, 16),
    ("Rings", rings, 84, 12),
    ("StarOfDavid", star, 84, 1),
    ("Synagogue", synagogue, 88, 1),
    ("Party", party, 84, 12),
    ("Baby", baby, 80, 1),
    ("GradCap", grad_cap, 88, 1),
    ("House", house, 84, 1),
    ("Calendar", calendar, 76, 1),
)


# ── output ───────────────────────────────────────────────────────────────────

def render(shade, width, frames):
    """Rows of (r, g, b), all frames side by side."""
    rows = [[(0, 0, 0)] * (width * frames) for _ in range(HEIGHT)]
    n = SS * SS
    offs = [(i + 0.5) / SS for i in range(SS)]
    for f in range(frames):
        t = f / frames
        for y in range(HEIGHT):
            row = rows[y]
            for x in range(width):
                r = g = b = 0
                for oy in offs:
                    for ox in offs:
                        c = shade(x + ox, y + oy, t)
                        if c:
                            r += c[0]; g += c[1]; b += c[2]
                row[f * width + x] = (r // n, g // n, b // n)
    return rows


def rgb565(c):
    v = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)
    return 0xF81E if v == 0xF81F else v                            # 0xF81F = transparent on the clock


def write_bmp(path, rows, frames):
    width, height = len(rows[0]), len(rows)
    pad = (4 - (width * 2) % 4) % 4
    pixels = bytearray()
    for row in reversed(rows):                                     # BMP rows go bottom to top
        pixels += struct.pack("<%dH" % width, *[rgb565(c) for c in row]) + b"\0" * pad
    offset = 14 + 40 + 12
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", offset + len(pixels), 0, 0, offset))
        f.write(struct.pack("<IIIHHIIIIII", 40, width, height, 1, 16, 3, len(pixels), frames, 0, 0, 0))
        f.write(struct.pack("<III", 0xF800, 0x07E0, 0x001F))
        f.write(pixels)


def write_png(path, rows, zoom):
    width, height = len(rows[0]) * zoom, len(rows) * zoom
    raw = bytearray()
    for row in rows:
        line = b"\0" + b"".join(bytes(c) * zoom for c in row)
        raw += line * zoom

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    sheet = []
    for name, shade, width, frames in ICONS:
        rows = render(shade, width, frames)
        path = os.path.join(OUT_DIR, name + ".bmp")
        write_bmp(path, rows, frames)
        print("%-15s %3dx%d, %2d frame(s), %6d bytes" % (name, width, HEIGHT, frames, os.path.getsize(path)))
        if preview:                                                # first frame and (if animated) two more
            picks = [0] if frames == 1 else [0, frames // 3, 2 * frames // 3]
            sheet.append([[px for f in picks for px in row[f * width:(f + 1) * width] + [(40, 40, 40)] * 4]
                          for row in rows])
    if preview:
        wide = max(len(r[0]) for r in sheet)
        rows = []
        for block in sheet:
            rows += [r + [(0, 0, 0)] * (wide - len(r)) for r in block] + [[(40, 40, 40)] * wide] * 4
        write_png(preview, rows, 2)
        print("preview:", preview)


if __name__ == "__main__":
    main()
