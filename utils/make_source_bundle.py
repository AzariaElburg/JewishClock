"""
The source code of everything the firmware is built from, as release files
(the user, 2026-11-04): if anyone asks for the source of an open-source part,
we can hand over exactly the version in the clock - whether or not its
authors still offer it.

    python utils/make_source_bundle.py          # sources-<version>.zip
    python utils/make_source_bundle.py --kit    # + build-kit-<version>-*.zip

-> <local_dirs.DIST>/sources/<version>/  - attach them to the GitHub release.

sources-<version>.zip (~40 MB): the libraries exactly as PlatformIO fetched
  and built them (libdeps) and the Arduino-ESP32 core's source (framework
  package) - this covers what the licenses ask for (LGPL: the core,
  ESPAsyncWebServer, AsyncTCP; the others only need their notice, which
  Data/licenses.txt carries). The font and SortableJS are in the project
  itself (assets/fonts, Data/).
build-kit-<version>-*.zip (~635 MB in parts, each under GitHub's 2 GB per
  file): everything else the build used - compiler, ESP-IDF's prebuilt
  libraries for the ESP32-S3, the platform and all its packages, and wheels
  of both Python environments (PlatformIO's and the platform's penv) with
  kit_setup.py - so the firmware can be built again with no download at all
  (tested 2026-11-05). Not required by any license. How to use it:
  BUILD-KIT-<version>.txt inside the first part.

Run it for every release, from the build that made the released firmware
(it reads the version from firmware.bin, like make_update_package.py).
"""
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from local_dirs import DIST, FIRMWARE_BUILD, LIBDEPS

ROOT = Path(__file__).resolve().parent.parent
PIO_CORE = Path("C:/pio3")                       # PLATFORMIO_CORE_DIR of the 2.x build (CLAUDE.md)
PIO_VENV = Path.home() / ".pioarduino" / "venv"
FIRMWARE = FIRMWARE_BUILD / "esp32s3" / "firmware.bin"
CHIP = "esp32s3"
PART_LIMIT = 1900 * 1024 * 1024                  # GitHub: under 2 GB per release file


def version():
    m = re.search(rb"JCVER=([0-9A-Za-z.\-]{1,40});", FIRMWARE.read_bytes())
    if not m:
        sys.exit("No version in %s - build first (pio run -c pioarduino.ini)." % FIRMWARE)
    return m.group(1).decode()


def package_version(d):
    for name in ("package.json", "library.json"):
        f = d / name
        if f.exists():
            try:
                return json.loads(f.read_text(encoding="utf-8")).get("version", "?")
            except Exception:
                pass
    f = d / "library.properties"
    if f.exists():
        m = re.search(r"^version=(.*)$", f.read_text(encoding="utf-8", errors="replace"), re.M)
        if m:
            return m.group(1).strip()
    return "?"


def add_tree(z, src, arc, skip=()):
    n = 0
    for f in sorted(src.rglob("*")):
        rel = f.relative_to(src)
        if f.is_file() and not any(p in skip for p in rel.parts):
            z.write(f, (Path(arc) / rel).as_posix())
            n += 1
    return n


def sources_zip(out, ver):
    libs = LIBDEPS / CHIP
    core = PIO_CORE / "packages" / "framework-arduinoespressif32"
    lines = ["Source code of the parts JewishClock %s is built from, exactly as built." % ver, "",
             "libraries/  (PlatformIO lib_deps, pioarduino.ini)"]
    target = out / ("sources-%s.zip" % ver)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d in sorted(p for p in libs.iterdir() if p.is_dir()):
            n = add_tree(z, d, "libraries/" + d.name, skip={".git", ".pio"})
            lines.append("  %-32s %-10s %5d files" % (d.name, package_version(d), n))
        n = add_tree(z, core, "framework-arduinoespressif32", skip={".git"})
        lines += ["", "framework-arduinoespressif32/  Arduino-ESP32 core %s, %d files" % (package_version(core), n),
                  "", "Licenses: see each folder, and Data/licenses.txt / THIRD_PARTY_LICENSES/ in the",
                  "project. Also in the project itself: the font (assets/fonts), SortableJS (Data/)."]
        z.writestr("README.txt", "\n".join(lines) + "\n")
    print("%s  %.1f MB" % (target, target.stat().st_size / 1e6))


KIT_README = """Build kit for JewishClock {ver}: everything the firmware was built with,
besides the project and sources-{ver}.zip - for building it again without
any download (PlatformIO, its packages or the libraries gone or changed).

Parts (unzip all into one folder, e.g. C:/pio3 - keep paths short, the
framework has ~200-character paths):
{parts}
Then (Windows, Python 3.10 to 3.13):
1. In the kit folder: python kit_setup.py
   (PlatformIO {pio} and the platform's Python packages, from python-wheels/)
2. Unzip sources-{ver}.zip's libraries/ into the project's libdeps folder
   (pioarduino.ini libdeps_dir, as <libdeps_dir>/esp32s3/<library>).
3. In pioarduino.ini point the platform at the local copy:
      platform = file://<kit>/platforms/espressif32
4. set PLATFORMIO_CORE_DIR=<kit>
   set PLATFORMIO_OFFLINE=1   (else the platform re-checks its Python
                               packages online on every build)
   <kit>/venv/Scripts/pio run -c pioarduino.ini
   PlatformIO finds packages/ there and downloads nothing.
Tested 2026-11-05: built from the kit alone, downloads blocked, Python 3.12.
Only the ESP32-S3 part of ESP-IDF's prebuilt libraries is included (the
clock's chip). ESP-IDF itself (Apache-2.0): https://github.com/espressif/esp-idf
at the commit in packages/framework-arduinoespressif32-libs/versions.txt.
"""


def kit_zips(out, ver):
    pk = PIO_CORE / "packages"
    groups = [
        ("compiler", [(pk / "toolchain-xtensa-esp-elf", "packages/toolchain-xtensa-esp-elf")]),
        ("idf-libs", [(pk / "framework-arduinoespressif32-libs" / CHIP, "packages/framework-arduinoespressif32-libs/" + CHIP),
                      (pk / "framework-arduinoespressif32-libs" / "hosted", "packages/framework-arduinoespressif32-libs/hosted")]
                     + [(f, "packages/framework-arduinoespressif32-libs/" + f.name)
                        for f in (pk / "framework-arduinoespressif32-libs").iterdir() if f.is_file()]),
        # Every other package the working install has: the platform checks
        # some on every build even when they aren't used (installer, gdb,
        # PIO Home) and downloads them if missing (kit test 2026-11-05).
        ("platform", [(PIO_CORE / "platforms" / "espressif32", "platforms/espressif32")]
                     + [(d, "packages/" + d.name) for d in sorted(pk.iterdir())
                        if d.is_dir() and d.name not in ("toolchain-xtensa-esp-elf", "framework-arduinoespressif32-libs")]),
    ]
    wheels = out / "_wheels"
    shutil.rmtree(wheels, ignore_errors=True)   # only this version's wheels
    pio_ver = subprocess.run([str(PIO_VENV / "Scripts" / "pip.exe"), "show", "pioarduino"],
                             capture_output=True, text=True).stdout
    pio_ver = re.search(r"^Version: (.*)$", pio_ver, re.M).group(1).strip()
    # Python packages for kit_setup.py: PlatformIO (venv) and the platform's
    # own environment (penv - exactly what the working one has; the platform
    # would fetch it online on every build). Wheels for every Python the
    # README names: one built for this PC's 3.10 only (charset_normalizer)
    # stopped the kit on 3.12 (kit test 2026-11-05).
    # Exact versions of both working environments (complete lists, so they
    # install with --no-deps; esptool comes from packages/tool-esptoolpy).
    pip = str(PIO_VENV / "Scripts" / "pip.exe")
    freeze = lambda py: [r for r in subprocess.run([str(py), "-m", "pip", "freeze"], capture_output=True, text=True,
                                                   check=True).stdout.split() if "==" in r]
    venv_reqs = freeze(PIO_VENV / "Scripts" / "python.exe")
    penv_reqs = freeze(PIO_CORE / "penv" / "Scripts" / "python.exe") + ["setuptools==80.9.0", "wheel==0.45.1"]  # esptool -e
    reqs = out / "_requirements.txt"
    reqs.write_text("\n".join(sorted(set(venv_reqs + penv_reqs))) + "\n", encoding="utf-8")
    for pyv in ("3.10", "3.11", "3.12", "3.13"):
        subprocess.run([pip, "download", "-q", "--no-deps", "-d", str(wheels), "-r", str(reqs), "--only-binary=:all:",
                        "--platform", "win_amd64", "--python-version", pyv], check=True)
    files = [(wheels, "python-wheels")]
    for name, text in (("venv-requirements.txt", "\n".join(venv_reqs) + "\n"),
                       ("penv-requirements.txt", "\n".join(penv_reqs) + "\n")):
        (out / ("_" + name)).write_text(text, encoding="utf-8")
        files.append((out / ("_" + name), name))
    files.append((ROOT / "utils" / "kit_setup.py", "kit_setup.py"))
    groups.append(("python", files))
    names = []
    for name, items in groups:
        part, k, z = None, 0, None
        size = 0
        for src, arc in items:
            files = [src] if src.is_file() else sorted(f for f in src.rglob("*") if f.is_file())
            for f in files:
                if z is None or size > PART_LIMIT:
                    if z:
                        z.close()
                    k += 1
                    part = out / ("build-kit-%s-%s-%d.zip" % (ver, name, k))
                    z, size = zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED, compresslevel=6), 0
                    names.append(part.name)
                a = arc if src.is_file() else (Path(arc) / f.relative_to(src)).as_posix()
                z.write(f, a)
                size += f.stat().st_size // 3          # rough compressed size
        z.close()
    first = out / names[0]
    readme = KIT_README.format(ver=ver, pio=pio_ver, parts="".join("  %s\n" % n for n in names))
    with zipfile.ZipFile(first, "a") as z:
        z.writestr("BUILD-KIT-" + ver + ".txt", readme)
    (out / ("BUILD-KIT-%s.txt" % ver)).write_text(readme, encoding="utf-8")
    for n in names:
        print("%s  %.0f MB" % (out / n, (out / n).stat().st_size / 1e6))


def main():
    ver = version()
    out = DIST / "sources" / ver
    out.mkdir(parents=True, exist_ok=True)
    sources_zip(out, ver)
    if "--kit" in sys.argv:
        kit_zips(out, ver)


if __name__ == "__main__":
    main()
