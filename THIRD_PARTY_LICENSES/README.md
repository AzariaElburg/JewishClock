# Third-party licenses

Exact license texts of everything in the firmware made by others (libraries,
framework parts, the font, SortableJS), copied from each project or from the
framework's own source headers (2026-10-31). `utils/make_licenses.py` turns
them into `Data/licenses.txt`, which the clock serves (Settings > Clock
software > Licenses) and every update package carries. The list of parts,
and the online data services with their terms, is in that script - edit it
there and rerun it when a library, the framework, the font or a service changes.

`CC-BY-4.0.txt` (2026-11-15): the license of the online data the clock shows
(Hebcal, Open-Meteo, GeoNames), in full - the clock only downloads that data,
but its credit line names the license, and a link alone relies on the internet
keeping the text. Copied from creativecommons.org/licenses/by/4.0/legalcode.txt.
The services' own terms of use (ip-api.com, Open-Meteo's "free for
non-commercial use") are web pages, not license texts: they can change at any
time and are only linked.

## The font

The clock's font (`include/Font.h`) is drawn from one external font. Its
exact, unmodified license text is kept here (from the font's own
repository) so the required copyright notices and license terms travel
with the source, not just as an external link that could go stale.

## Noto Sans Hebrew (all characters: Latin, digits, Hebrew)

- File: `NotoSansHebrew-OFL.txt` (SIL Open Font License 1.1)
- Source: https://github.com/notofonts/hebrew (font file used:
  `assets/fonts/NotoSansHebrew-Variable.ttf`, from Google Fonts' repository)
- Copyright 2022 The Noto Project Authors
- `utils/generate_font.py` renders it (Bold, narrowed, the colon smaller)
  into the clock's own 4-bit pixel format - since 2026-10-21. Before that:
  Assistant ExtraBold (2026-10-20, OFL), and before that the pixel fonts
  Spleen (Latin, BSD 2-Clause) and Terminus (Hebrew, OFL); their files left
  with them (git history has all of them).

## What this means in practice for this project

The OFL explicitly permits what this project does: converting the glyphs
into a custom pixel-data format and embedding them in firmware, including
firmware that gets built, flashed, shared, or otherwise distributed. Two
things to keep in mind going forward:

- **Don't distribute the converted glyph data (or this repo's `Font.h`) as
  a standalone "font product" on its own.** Embedded inside this project
  (or any larger project) is fine.
- **If the glyphs are ever turned back into an installable font file and
  redistributed**, check the OFL's Reserved Font Name rules for the name it
  may carry.

Keep this folder, and the attribution comment at the top of `include/Font.h`,
intact if the font data in `Font.h` is ever changed again.
