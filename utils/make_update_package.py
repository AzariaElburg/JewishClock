"""
Builds the single update file for the clock: firmware + web files.

    python utils/make_update_package.py

Run after `pio run` (it packs firmware.bin as built - local_dirs.FIRMWARE_BUILD).
Writes <local_dirs.DIST>/JewishClock-update-<version>.jcupdate - the one file to email.
It is installed from the clock's settings page: Firmware Update -> choose
the file -> Update. Settings, events and the log on the clock are never in
the package and never touched. Format and safety net: include/Updater.h.

Package format (all integers little-endian, crc32 = zlib.crc32):
  b"JCUPD1" | u8 versionLen | version | u16 entryCount
  entryCount x ( u8 type (1 = web file, 2 = firmware) | u8 pathLen | path |
                 u32 size | u32 crc32 | data )
  b"JCEND!"
"""
import re
import struct
import sys
import zlib
from pathlib import Path

from local_dirs import FIRMWARE_BUILD, DIST  # outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = FIRMWARE_BUILD / "esp32s3" / "firmware.bin"
DATA = ROOT / "Data"

# The clock's own data - never shipped (Updater.cpp refuses them too).
EXCLUDE = {"config.json", "events.json", "diagnostic_log.txt", "diagnostic_log.old.txt"}
# Event pictures (FS_IMAGE_DIR in Config.h; "photos" = the 2026-10-19..21
# folder) and the family's replacements of the clock's own pictures
# (FS_CUSTOM_DIR) - in Data/ only to survive an uploadfs, never shipped.
EXCLUDE_DIRS = {"images", "photos", "custom", "log"}


def main():
    if not FIRMWARE.exists():
        sys.exit(f"No firmware at {FIRMWARE} - run `pio run` first.")
    fw = FIRMWARE.read_bytes()
    # "2.0.1" or e.g. "2.0.1-3-g1a2b3c4-dirty" (scripts/gen_version.py).
    m = re.search(rb"JCVER=([0-9A-Za-z.\-]{1,40});", fw)
    if not m:
        sys.exit("Version marker not found in firmware.bin - is it a current build?")
    version = m.group(1).decode()

    # A failed `pio run` leaves the previous firmware.bin in place - never
    # package that stale file by mistake: no firmware source may be newer.
    built = FIRMWARE.stat().st_mtime
    sources = [p for d in ("src", "include", "lib", "scripts") for p in (ROOT / d).rglob("*") if p.is_file()]
    sources.append(ROOT / "pioarduino.ini")
    newer = [p for p in sources if p.stat().st_mtime > built]
    if newer:
        sys.exit(f"firmware.bin ({version}) is older than {newer[0].relative_to(ROOT)} - the last build "
                 f"probably failed. Run `pio run -c pioarduino.ini` again and check it says [SUCCESS].")

    entries = []
    for f in sorted(DATA.rglob("*")):
        if not f.is_file() or f.name in EXCLUDE or f.relative_to(DATA).parts[0] in EXCLUDE_DIRS:
            continue
        rel = "/" + f.relative_to(DATA).as_posix()
        entries.append((1, rel, f.read_bytes()))
    entries.append((2, "firmware.bin", fw))

    out = bytearray(b"JCUPD1")
    out += struct.pack("<B", len(version)) + version.encode()
    out += struct.pack("<H", len(entries))
    for kind, path, data in entries:
        p = path.encode("utf-8")
        if not 0 < len(p) <= 120:
            sys.exit(f"Path too long for the package format: {path}")
        out += struct.pack("<BB", kind, len(p)) + p
        out += struct.pack("<II", len(data), zlib.crc32(data) & 0xFFFFFFFF)
        out += data
    out += b"JCEND!"

    DIST.mkdir(parents=True, exist_ok=True)
    target = DIST / f"JewishClock-update-{version}.jcupdate"
    target.write_bytes(out)
    web = [e for e in entries if e[0] == 1]
    print(f"Version {version}: firmware {len(fw):,} bytes + {len(web)} web files "
          f"({sum(len(e[2]) for e in web):,} bytes)")
    for _, path, data in web:
        print(f"  {path}  {len(data):,}")
    print(f"-> {target}  ({len(out):,} bytes)")


if __name__ == "__main__":
    main()
