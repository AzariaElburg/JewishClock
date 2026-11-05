import math
import struct


def pack_rgb565(r, g, b):
    """Converts 8-bit R, G, B (0-255) into 16-bit RGB565 integer (5-6-5 bits)."""
    r_5 = (r >> 3) & 0x1F
    g_6 = (g >> 2) & 0x3F
    b_5 = (b >> 3) & 0x1F
    return (r_5 << 11) | (g_6 << 5) | b_5


def create_dual_candle_bmp(filename="dual_candle_anim.bmp"):
    # Target Specifications
    frame_width = 112  # Expanded width to comfortably fit two candles
    frame_height = 106
    frame_count = 16

    total_width = frame_width * frame_count
    total_height = frame_height

    bpp = 16  # 16bpp RGB565

    # BMP Row padding (padded to multiple of 4 bytes)
    row_bytes_unpadded = total_width * 2
    padding_bytes = (4 - (row_bytes_unpadded % 4)) % 4
    row_bytes_total = row_bytes_unpadded + padding_bytes
    pixel_array_size = row_bytes_total * total_height

    # BI_BITFIELDS Header configuration for RGB565
    header_size = 54
    color_masks_size = 12
    pixel_offset = header_size + color_masks_size
    file_size = pixel_offset + pixel_array_size

    # Canvas initialization [height][width]
    canvas = [[0 for _ in range(total_width)] for _ in range(total_height)]

    # Candle positions relative to frame center
    candle_centers_offset = [-22.0, 22.0]

    # Flame, wick and wax top raised by this many rows. 17 = the tallest flame
    # (frame 3) touches the top row (the user, 2026-11-04: flame to the top).
    lift_rows = 17
    # The wax ends above this row, the rest of the frame stays empty (the
    # picture keeps its full height, so the flame stays at the window's top).
    # 96: halfway between wax to the bottom (dwarfed the time next to it) and
    # 85 (~3 rows below the digits on a 97-row window - too short); the user,
    # 2026-11-04. Another window height moves it a little against the text.
    wax_bottom_y = 96

    for f in range(frame_count):
        frame_offset_x = f * frame_width
        frame_center_x = frame_offset_x + (frame_width / 2.0)

        # Loop phase variable: 0.0 to 2*PI across the 16 frames
        t = f * (2.0 * math.pi / frame_count)

        # Shared macro-airflow wave (drives primary motion synchronization)
        ambient_drift = math.sin(t) * 2.5 + math.sin(t * 2) * 1.0

        for c_idx, offset in enumerate(candle_centers_offset):
            center_x = frame_center_x + offset

            # --- Flame Motion & Offset Variations ---
            if c_idx == 0:  # Left Candle
                sway_x = ambient_drift + math.cos(t * 2) * 0.6
                flame_height_mod = (
                    math.sin(t * 2) * 2.5 + math.cos(t * 4) * 1.0
                )
                max_w = 10.0
            else:  # Right Candle (Phase-shifted & slightly slimmer/taller)
                # Phase shift (+0.45 rad) creates natural synchronized lag/lead
                sway_x = (
                    math.sin(t + 0.45) * 2.3
                    + math.sin((t + 0.45) * 2) * 1.1
                    + math.sin(t * 3) * 0.5
                )
                flame_height_mod = (
                    math.cos(t * 2 + 0.3) * 3.0 + math.sin(t * 4) * 0.8
                )
                max_w = 9.5

            wick_x = center_x
            wick_top_y = 66.0 - lift_rows
            wax_top_y = 78.0 - lift_rows

            # Render Pixel Grid for current candle
            for y in range(total_height):
                # Search window restricted around current candle center
                x_start = max(0, int(center_x - 30))
                x_end = min(total_width, int(center_x + 30))

                for px in range(x_start, x_end):

                    # --- 1. Wax Body & Rim ---
                    if y >= wax_bottom_y:
                        continue
                    if y >= wax_top_y:
                        dx = abs(px - center_x)
                        if dx < 16:
                            shade = math.cos(
                                (px - center_x) / 16.0 * (math.pi / 2)
                            )
                            depth = (y - wax_top_y) / (
                                wax_bottom_y - wax_top_y
                            )
                            r = int(
                                (210 * shade + 30) * (1.0 - depth * 0.3)
                                + (90 * depth)
                            )
                            g = int((180 * shade + 20) * (1.0 - depth * 0.4))
                            b = int((130 * shade + 10) * (1.0 - depth * 0.5))

                            # Overwrite pixel
                            canvas[y][px] = pack_rgb565(
                                min(255, r), min(255, g), min(255, b)
                            )
                            continue

                    # --- 2. Candle Wick ---
                    if y >= wick_top_y and y < wax_top_y:
                        dx = abs(px - wick_x)
                        if dx <= 1.0:
                            ember = 1.0 if y < wick_top_y + 3 else 0.0
                            r = int(255 * ember + 40 * (1 - ember))
                            g = int(80 * ember + 30 * (1 - ember))
                            b = int(20 * ember + 30 * (1 - ember))
                            canvas[y][px] = pack_rgb565(r, g, b)
                            continue

                    # --- 3. Dynamic Flame ---
                    if y < wax_top_y:
                        flame_base_y = wax_top_y - 2
                        flame_top_y = 20.0 - lift_rows + flame_height_mod
                        flame_len = flame_base_y - flame_top_y

                        if flame_top_y <= y <= flame_base_y:
                            fy = (
                                flame_base_y - y
                            ) / flame_len  # 0.0 (base) -> 1.0 (top)

                            # Lean curve intensifies toward flame tip
                            curved_center = center_x + sway_x * (fy**1.25)

                            # Teardrop thickness profile
                            if fy < 0.22:
                                w = max_w * (fy / 0.22) ** 0.65
                            else:
                                w = max_w * ((1.0 - fy) / 0.78) ** 1.15

                            dx = abs(px - curved_center)

                            if dx <= w + 2.5:
                                dist = dx / max(0.001, w)

                                # Multi-zone flame shading
                                if dist < 0.32 and fy < 0.65:
                                    if fy < 0.18:
                                        r, g, b = (
                                            20,
                                            40,
                                            220,
                                        )  # Blue base ignition
                                    else:
                                        r, g, b = (
                                            255,
                                            252,
                                            230,
                                        )  # White core
                                elif dist < 0.72:
                                    core_ratio = (dist - 0.32) / 0.40
                                    r = 255
                                    g = int(240 - 95 * core_ratio)
                                    b = int(90 - 90 * core_ratio)
                                elif dist < 1.0:
                                    edge_ratio = (dist - 0.72) / 0.28
                                    r = int(255 - 45 * edge_ratio)
                                    g = int(145 - 110 * edge_ratio)
                                    b = 10
                                else:
                                    fade = max(0.0, 1.0 - (dist - 1.0) / 0.4)
                                    r = int(180 * fade)
                                    g = int(45 * fade)
                                    b = 0

                                # Write color to canvas
                                canvas[y][px] = pack_rgb565(
                                    min(255, max(0, r)),
                                    min(255, max(0, g)),
                                    min(255, max(0, b)),
                                )

    # --- Assemble BMP Headers ---
    file_header = struct.pack(
        "<2sIHHI", b"BM", file_size, 0, 0, pixel_offset
    )

    dib_header = struct.pack(
        "<IIIHHIIIIII",
        40,
        total_width,
        total_height,
        1,
        bpp,
        3,  # BI_BITFIELDS
        pixel_array_size,
        frame_count,  # biXPelsPerMeter = 16
        0,
        0,
        0,
    )

    bit_masks = struct.pack("<III", 0xF800, 0x07E0, 0x001F)

    # Convert scanlines to Bottom-to-Top sequence
    pixel_bytes = bytearray()
    padding = b"\x00" * padding_bytes

    for y in range(total_height - 1, -1, -1):
        row_str = bytearray()
        for x in range(total_width):
            row_str.extend(struct.pack("<H", canvas[y][x]))
        pixel_bytes.extend(row_str)
        pixel_bytes.extend(padding)

    # Save output file
    with open(filename, "wb") as f:
        f.write(file_header)
        f.write(dib_header)
        f.write(bit_masks)
        f.write(pixel_bytes)

    print(
        f"Generated: {filename}\n"
        f"  Total Image Size : {total_width}x{total_height} px\n"
        f"  Single Frame Size: {frame_width}x{frame_height} px\n"
        f"  Frame Count      : {frame_count} (in biXPelsPerMeter)\n"
        f"  Looping          : 100% mathematically seamless"
    )


if __name__ == "__main__":
    create_dual_candle_bmp()