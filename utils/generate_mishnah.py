"""
Draw the Mishna Yomit picture from scratch (2026-11-04): a closed hardcover
volume, front cover with a gold frame and the title "משנה", the page edges
showing at the side and bottom. Replaces a photo of a real book's cover
(no reuse license; the original is kept in assets/images/ only, never on
the clock).

Still picture, 16bpp RGB565 BMP like the other generators. Candidates +
an enlarged preview to C:\\pio3\\jc_clean\\mishnah\\ (local_dirs.py) - copy the
chosen one to Data/IMG/Mishnah.bmp.

    python utils/generate_mishnah.py          # all candidates
    python utils/generate_mishnah.py --h 97   # for a 97-row window

Make it exactly the display window's height (calibration H) - then the clock
shows it pixel for pixel. The title uses the clock's own font file (Noto
Sans Hebrew, OFL - THIRD_PARTY_LICENSES/).
"""
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_dirs  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
FONT_FILE = ROOT / "assets" / "fonts" / "NotoSansHebrew-Variable.ttf"

# === TUNABLES ===
FRAME_H = 97            # default; --h N. The final box's window height (2026-11-04)
SS = 4                  # drawn this many times larger, then shrunk (smooth edges)
PREVIEW_SCALE = 5
GOLD = (222, 180, 70)
PAGES = (235, 225, 200)
TITLE = "משנה"

# cover: colour; w: cover width as a share of the height; title_px: title
# height in rows (at FRAME_H 97)
CANDIDATES = {
    "M1": dict(cover=(128, 28, 36), w=0.70, title_px=17),
    "M2": dict(cover=(30, 50, 110), w=0.70, title_px=17),
}


def render(cfg):
    H = FRAME_H * SS
    W = int(round(FRAME_H * cfg["w"])) * SS
    s = lambda v: int(round(v * FRAME_H / 97 * SS))   # rows at FRAME_H 97 -> drawing px
    im = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(im)
    edge = s(5)                                        # page edges seen at the side and bottom
    cover = (0, s(1), W - edge, H - edge - s(1))
    # the pages: a cream block behind the cover, offset down-right, with thin lines
    d.rounded_rectangle((edge, edge + s(1), W - 1, H - s(1)), radius=s(3), fill=PAGES)
    for k in range(1, 4):
        y = H - s(1) - k * edge // 4
        d.line((edge + s(4), y, W - s(2), y), fill=(180, 168, 140), width=max(1, SS // 2))
        x = W - 1 - k * edge // 4
        d.line((x, edge + s(5), x, H - s(4)), fill=(180, 168, 140), width=max(1, SS // 2))
    # the cover, its spine shadow on the right edge of the binding
    d.rounded_rectangle(cover, radius=s(3), fill=cfg["cover"])
    d.rectangle((cover[0], cover[1] + s(2), cover[0] + s(4), cover[3] - s(2)),
                fill=tuple(int(c * 0.7) for c in cfg["cover"]))
    # gold frame
    m = s(9)
    d.rounded_rectangle((cover[0] + m, cover[1] + m, cover[2] - m + s(2), cover[3] - m),
                        radius=s(2), outline=GOLD, width=s(2))
    # title, in the middle of the frame, as large as fits inside it (Pillow
    # here has libraqm: it lays Hebrew out right to left by itself)
    text, inner_w = TITLE, (cover[2] - cover[0]) - 2 * m - s(6)
    size = s(cfg["title_px"]) * 13 // 10
    while True:
        font = ImageFont.truetype(str(FONT_FILE), size)
        try:
            font.set_variation_by_name("Bold")
        except Exception:
            pass
        box = d.textbbox((0, 0), text, font=font)
        tw, th = box[2] - box[0], box[3] - box[1]
        if tw <= inner_w or size <= SS * 6:
            break
        size -= SS
    cx = (cover[0] + s(4) + cover[2]) / 2
    cy = (cover[1] + cover[3]) / 2
    d.text((cx - tw / 2 - box[0], cy - th / 2 - box[1]), text, font=font, fill=GOLD)
    # small ornaments above and below the title
    for oy in (cy - th / 2 - s(9), cy + th / 2 + s(9)):
        d.line((cx - s(10), oy, cx + s(10), oy), fill=GOLD, width=s(1.5))
        d.ellipse((cx - s(2), oy - s(2), cx + s(2), oy + s(2)), fill=GOLD)
    small = im.resize((W // SS, FRAME_H), Image.LANCZOS)
    return [[small.getpixel((x, y)) for x in range(small.width)] for y in range(small.height)]


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
    out_dir = local_dirs.LOCAL / "mishnah"
    out_dir.mkdir(parents=True, exist_ok=True)
    tiles = []
    for name, cfg in CANDIDATES.items():
        frame = render(cfg)
        write_bmp(out_dir / f"Mishnah_{name}.bmp", frame)
        im = Image.new("RGB", (len(frame[0]), len(frame)))
        im.putdata([p for row in frame for p in row])
        tiles.append((name, im))
        print(f"{name}: {im.width}x{im.height}")
    gap = 20
    sheet = Image.new("RGB", (sum(t.width * PREVIEW_SCALE + gap for _, t in tiles) + gap,
                              FRAME_H * PREVIEW_SCALE + 40), (60, 60, 60))
    d = ImageDraw.Draw(sheet)
    x = gap
    for name, t in tiles:
        d.text((x, 8), f"{name} {t.width}x{t.height}", fill=(255, 255, 255))
        sheet.paste(t.resize((t.width * PREVIEW_SCALE, t.height * PREVIEW_SCALE), Image.NEAREST), (x, 30))
        x += t.width * PREVIEW_SCALE + gap
    sheet.save(out_dir / "candidates.png")
    print("Preview:", out_dir / "candidates.png")


if __name__ == "__main__":
    main()
