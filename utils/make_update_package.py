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
  then the signature (2026-11-14; versions before it stop reading at JCEND! and ignore it):
  b"JCSIG1" | u16 sigLen | ECDSA P-256 signature (DER) of SHA-256 over every byte up to and
  including JCEND! - made with the private key (utils/update_key.py); the clock checks it
  with the public key in its firmware (include/UpdateKey.h) and installs nothing without it.
"""
import re
import struct
import sys
import zlib
from pathlib import Path

from local_dirs import FIRMWARE_BUILD, DIST  # outside the OneDrive folder
from web_files import clock_files
ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = FIRMWARE_BUILD / "esp32s3" / "firmware.bin"
DATA = ROOT / "Data"

# The clock's own data (settings, events, log, pictures...) - never shipped:
# the firmware's own list (Config.h isClockDataPath(), read by
# clock_data_paths.py - this script kept a copy until 2026-11-08). In Data/
# only to survive an uploadfs.
from clock_data_paths import is_clock_data_path


def main():
    # --out DIR: written there instead of DIST (test runs: never next to the packages that get emailed)
    out_dir = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else DIST
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
    for rel, data, f in clock_files(DATA):   # the page files gzipped, as uploadfs puts them
        if is_clock_data_path(rel):
            continue
        entries.append((1, "/" + rel, data))
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
    import update_key
    sig = update_key.sign(bytes(out))
    if not update_key.verify(bytes(out), sig, update_key.header_public_der()):   # the key the firmware carries
        sys.exit("The signature does not check out against include/UpdateKey.h (another key?) - not written")
    out += b"JCSIG1" + struct.pack("<H", len(sig)) + sig

    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"JewishClock-update-{version}.jcupdate"
    target.write_bytes(out)
    web = [e for e in entries if e[0] == 1]
    print(f"Version {version}: firmware {len(fw):,} bytes + {len(web)} web files "
          f"({sum(len(e[2]) for e in web):,} bytes)")
    for _, path, data in web:
        print(f"  {path}  {len(data):,}")
    print(f"-> {target}  ({len(out):,} bytes, signed: key {update_key.key_id(update_key.public_der())})")


if __name__ == "__main__":
    main()
